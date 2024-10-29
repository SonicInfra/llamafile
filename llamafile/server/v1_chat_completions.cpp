// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi
//
// Copyright 2024 Mozilla Foundation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "client.h"

#include <math.h>
#include <string.h>
#include <sys/resource.h>
#include <vector>

#include "llama.cpp/llama.h"
#include "llama.cpp/sampling.h"
#include "llamafile/macros.h"
#include "llamafile/string.h"

#include "cleanup.h"
#include "fastjson.h"
#include "json.h"
#include "log.h"
#include "model.h"
#include "slot.h"
#include "utils.h"

struct V1ChatCompletionParams
{
    bool stream = false;
    long max_tokens = -1;
    long seed = _rand64();
    double top_p = 1;
    double temperature = 1;
    double presence_penalty = 0;
    double frequency_penalty = 0;
    std::string user;
    std::string model;
    std::vector<llama_chat_msg> messages;
    std::vector<std::vector<llama_token>> stop;
    std::string grammar;

    void add_stop(const std::string& text)
    {
        stop.emplace_back(llama_tokenize(g_model, text, false, false));
    }
};

struct V1ChatCompletionResponse
{
    std::string prompt;
    std::vector<llama_token> tokens;
    std::string content;
    Json json;
};

static bool
is_legal_role(const std::string_view& role)
{
    return role == "system" || //
           role == "user" || //
           role == "assistant";
}

static void
cleanup_params(void* arg)
{
    delete (V1ChatCompletionParams*)arg;
}

static void
cleanup_response(void* arg)
{
    delete (V1ChatCompletionResponse*)arg;
}

static void
cleanup_sampler(void* arg)
{
    llama_sampling_free((llama_sampling_context*)arg);
}

static void
cleanup_slot(void* arg)
{
    delete (Slot*)arg;
}

static std::string
generate_id()
{
    std::string b = "chatcmpl-";
    for (int i = 0; i < 2; ++i) {
        uint64_t w = _rand64();
        for (int j = 0; j < 64 / 5; ++j) {
            b += "abcdefghijklmnopqrstuvwxyz012345"[w & 31];
            w >>= 5;
        }
    }
    return b;
}

static llama_sampling_context*
create_sampler(const V1ChatCompletionParams* params)
{
    llama_sampling_params sparams;
    sparams.temp = params->temperature;
    sparams.top_p = params->top_p;
    sparams.penalty_freq = params->frequency_penalty;
    sparams.penalty_present = params->presence_penalty;
    sparams.seed = params->seed;
    sparams.grammar = params->grammar;
    return llama_sampling_init(sparams);
}

static std::string
make_event(const Json& json)
{
    std::string s = "data: ";
    s += json.toString();
    s += "\n\n";
    return s;
}

bool
Client::get_v1_chat_completions_params(V1ChatCompletionParams* params)
{
    // must be json post request
    if (msg.method != kHttpPost)
        return send_error(405);
    if (!HasHeader(kHttpContentType) ||
        !IsMimeType(HeaderData(kHttpContentType),
                    HeaderLength(kHttpContentType),
                    "application/json")) {
        return send_error(501, "Content Type Not Implemented");
    }
    if (!read_payload())
        return false;

    // object<model, messages, ...>
    std::pair<Json::Status, Json> json = Json::parse(payload);
    if (json.first != Json::success)
        return send_error(400, Json::StatusToString(json.first));
    if (!json.second.isObject())
        return send_error(400, "JSON body must be an object");

    // fields openai documents that we don't support yet
    if (!json.second["n"].isNull())
        return send_error(400, "OpenAI n field not supported");
    if (!json.second["tools"].isNull())
        return send_error(400, "OpenAI tools field not supported");
    if (!json.second["audio"].isNull())
        return send_error(400, "OpenAI audio field not supported");
    if (!json.second["logprobs"].isNull())
        return send_error(400, "OpenAI logprobs field not supported");
    if (!json.second["functions"].isNull())
        return send_error(400, "OpenAI functions field not supported");
    if (!json.second["modalities"].isNull())
        return send_error(400, "OpenAI modalities field not supported");
    if (!json.second["tool_choice"].isNull())
        return send_error(400, "OpenAI tool_choice field not supported");
    if (!json.second["top_logprobs"].isNull())
        return send_error(400, "OpenAI top_logprobs field not supported");
    if (!json.second["function_call"].isNull())
        return send_error(400, "OpenAI function_call field not supported");
    if (!json.second["parallel_tool_calls"].isNull())
        return send_error(400, "parallel_tool_calls field not supported");

    // model: string
    Json& model = json.second["model"];
    if (!model.isString())
        return send_error(400, "JSON missing model string");
    params->model = std::move(model.getString());

    // messages: array<object<role:string, content:string>>
    if (!json.second["messages"].isArray())
        return send_error(400, "JSON missing messages array");
    std::vector<Json>& messages = json.second["messages"].getArray();
    for (Json& message : messages) {
        if (!message.isObject())
            return send_error(400, "messages array must hold objects");
        if (!message["role"].isString())
            return send_error(400, "message must have string role");
        if (!is_legal_role(message["role"].getString()))
            return send_error(400, "message role not system user assistant");
        if (!message["content"].isString())
            return send_error(400, "message must have string content");
        params->messages.emplace_back(
          std::move(message["role"].getString()),
          std::move(message["content"].getString()));
    }

    // stream: bool|null
    //
    // If set, partial message deltas will be sent, like in ChatGPT.
    // Tokens will be sent as data-only server-sent events as they
    // become available, with the stream terminated by a data: [DONE]
    // message.
    Json& stream = json.second["stream"];
    if (!stream.isNull()) {
        if (!stream.isBool())
            return send_error(400, "stream field must be boolean");
        params->stream = stream.getBool();
    }

    // max_tokens: integer|null
    //
    // An upper bound for the number of tokens that can be generated for
    // a completion. This can be used to control compute costs.
    Json& max_tokens = json.second["max_tokens"];
    if (!max_tokens.isNull()) {
        if (!max_tokens.isLong())
            return send_error(400, "max_tokens must be integer");
        params->max_tokens = max_tokens.getLong();
    }
    Json& max_completion_tokens = json.second["max_completion_tokens"];
    if (!max_completion_tokens.isNull()) {
        if (!max_completion_tokens.isLong())
            return send_error(400, "max_completion_tokens must be integer");
        params->max_tokens = max_completion_tokens.getNumber();
    }

    // top_p: number|null
    //
    // An alternative to sampling with temperature, called nucleus
    // sampling, where the model considers the results of the tokens
    // with top_p probability mass. So 0.1 means only the tokens
    // comprising the top 10% probability mass are considered.
    //
    // We generally recommend altering this or temperature but not both.
    Json& top_p = json.second["top_p"];
    if (!top_p.isNull()) {
        if (!top_p.isNumber())
            return send_error(400, "top_p must be number");
        params->top_p = top_p.getNumber();
    }

    // temperature: number|null
    //
    // What sampling temperature to use, between 0 and 2. Higher values
    // like 0.8 will make the output more random, while lower values
    // like 0.2 will make it more focused and deterministic.
    //
    // We generally recommend altering this or top_p but not both.
    Json& temperature = json.second["temperature"];
    if (!temperature.isNull()) {
        if (!temperature.isNumber())
            return send_error(400, "temperature must be number");
        params->temperature = temperature.getNumber();
        if (!(0 <= params->temperature && params->temperature <= 2))
            return send_error(400, "temperature must be between 0 and 2");
    }

    // seed: integer|null
    //
    // If specified, our system will make a best effort to sample
    // deterministically, such that repeated requests with the same seed
    // and parameters should return the same result. Determinism is not
    // guaranteed, and you should refer to the system_fingerprint
    // response parameter to monitor changes in the backend.
    Json& seed = json.second["seed"];
    if (!seed.isNull()) {
        if (!seed.isLong())
            return send_error(400, "seed must be integer");
        params->seed = seed.getLong();
    }

    // presence_penalty: number|null
    //
    // Number between -2.0 and 2.0. Positive values penalize new tokens
    // based on whether they appear in the text so far, increasing the
    // model's likelihood to talk about new topics.
    Json& presence_penalty = json.second["presence_penalty"];
    if (!presence_penalty.isNull()) {
        if (!presence_penalty.isNumber())
            return send_error(400, "presence_penalty must be number");
        params->presence_penalty = presence_penalty.getNumber();
        if (!(-2 <= params->presence_penalty && params->presence_penalty <= 2))
            return send_error(400, "presence_penalty must be between -2 and 2");
    }

    // frequency_penalty: number|null
    //
    // Number between -2.0 and 2.0. Positive values penalize new tokens
    // based on their existing frequency in the text so far, decreasing
    // the model's likelihood to repeat the same line verbatim.
    Json& frequency_penalty = json.second["frequency_penalty"];
    if (!frequency_penalty.isNull()) {
        if (!frequency_penalty.isNumber())
            return send_error(400, "frequency_penalty must be number");
        params->frequency_penalty = frequency_penalty.getNumber();
        if (!(-2 <= params->frequency_penalty &&
              params->frequency_penalty <= 2))
            return send_error(400, "frequency_penalty must be -2 through 2");
    }

    // user: string|null
    //
    // A unique identifier representing your end-user, which can help
    // llamafiler to monitor and detect abuse.
    Json& user = json.second["user"];
    if (!user.isNull()) {
        if (!user.isString())
            return send_error(400, "JSON missing user string");
        params->user = std::move(user.getString());
    }

    // stop: string|array<string>|null
    //
    // Up to 4 sequences where the API will stop generating further tokens.
    Json& stop = json.second["stop"];
    if (!stop.isNull()) {
        if (stop.isString()) {
            params->add_stop(stop.getString());
        } else if (stop.isArray()) {
            std::vector<Json>& stops = stop.getArray();
            if (stops.size() > 4)
                return send_error(400, "stop array must have 4 items or fewer");
            for (Json& stop2 : stops) {
                if (!stop2.isString())
                    return send_error(400, "stop array item must be string");
                if (stop2.getString().size() > 50)
                    return send_error(400, "stop array string too long");
                params->add_stop(stop2.getString());
            }
        } else {
            return send_error(400, "stop field must be string or string array");
        }
    }

    // response_format: "auto"
    // response_format: { "type": "json_object" }
    // response_format: { "type": "json_schema", "json_schema": {...} }
    //
    // An object specifying the format that the model must output.
    //
    // Setting to { "type": "json_schema", "json_schema": {...} }
    // enables Structured Outputs which ensures the model will match
    // your supplied JSON schema. Learn more in the Structured Outputs
    // guide.
    //
    // Setting to { "type": "json_object" } enables JSON mode, which
    // ensures the message the model generates is valid JSON.
    //
    // When using JSON mode, you must also instruct the model to produce
    // JSON yourself via a system or user message. Without this, the
    // model may generate an unending stream of whitespace until the
    // generation reaches the token limit, resulting in a long-running
    // and seemingly "stuck" request. Also note that the message content
    // may be partially cut off if finish_reason = "length", which
    // indicates the generation exceeded max_tokens or the conversation
    // exceeded the max context length.
    Json& response_format = json.second["response_format"];
    if (!response_format.isNull()) {
        if (response_format.isString()) {
            if (response_format.getString() != "auto")
                return send_error(400, "response_format not supported");
        } else if (response_format.isObject()) {
            Json& type = response_format.getObject()["type"];
            if (!type.isString())
                return send_error(400, "response_format.type must be string");
            if (type.getString() == "json_object") {
                params->grammar =
                  json_schema_string_to_grammar("{\"type\": \"object\"}");
            } else if (type.getString() == "json_schema") {
                Json& json_schema = response_format.getObject()["json_schema"];
                if (!json_schema.isObject())
                    return send_error(
                      400, "response_format.json_schema must be object");
                try {
                    params->grammar =
                      json_schema_string_to_grammar(json_schema.toString());
                } catch (const std::exception& e) {
                    SLOG("error: couldn't compile json schema: %s", e.what());
                    return send_error(400, "bad json schema");
                }
            } else {
                return send_error(400, "response_format.type unsupported");
            }
        } else {
            return send_error(400, "response_format must be string or object");
        }
    }

    return true;
}

bool
Client::v1_chat_completions()
{
    // get parameters
    auto params = new V1ChatCompletionParams;
    defer_cleanup(cleanup_params, params);
    if (!get_v1_chat_completions_params(params))
        return false;

    // create response object
    V1ChatCompletionResponse* response = new V1ChatCompletionResponse;
    defer_cleanup(cleanup_response, response);

    // turn text into tokens
    response->prompt =
      llama_chat_apply_template(g_model, "", params->messages, true);
    response->tokens = llama_tokenize(g_model, response->prompt, true, false);

    // find appropriate slot
    Slot* slot;
    slot = new Slot;
    if (!slot->start()) {
        delete slot;
        SLOG("failed to create slot");
        return send_error(500);
    }
    defer_cleanup(cleanup_slot, slot);

    // sanity check
    if (response->tokens.size() + 1 > slot->n_ctx())
        return send_error(400, "prompt too big for model context size");

    // init sampling
    llama_sampling_context* sampler = create_sampler(params);
    if (!sampler)
        return send_error(500, "failed to create sampler");
    defer_cleanup(cleanup_sampler, sampler);

    // prefill time
    if (!slot->prefill(response->tokens)) {
        SLOG("slot prefill failed");
        return send_error(500, "llama_decode prefill failed");
    }

    // setup response json
    response->json["id"].setString(generate_id());
    response->json["object"].setString("chat.completion");
    response->json["model"].setString(params->model);
    response->json["choices"].setArray();
    Json& choice = response->json["choices"][0];
    choice.setObject();
    choice["index"].setLong(0);
    choice["logprobs"].setNull();
    choice["finish_reason"].setNull();

    // initialize response
    if (params->stream) {
        char* p = append_http_response_message(obuf.p, 200);
        p = stpcpy(p, "Content-Type: text/event-stream\r\n");
        if (!send_response_start(obuf.p, p))
            return false;
        choice["delta"].setObject();
        choice["delta"]["role"].setString("assistant");
        choice["delta"]["content"].setString("");
        response->json["created"].setLong(timespec_real().tv_sec);
        response->content = make_event(response->json);
        choice.getObject().erase("delta");
        if (!send_response_chunk(response->content))
            return false;
    }

    // prediction time
    int completion_tokens = 0;
    const char* finish_reason = "length";
    while (params->max_tokens < 0 || completion_tokens < params->max_tokens) {
        llama_token id = llama_sampling_sample(sampler, slot->ctx_, NULL);
        llama_sampling_accept(sampler, slot->ctx_, id, true);
        if (llama_token_is_eog(g_model, id)) {
            finish_reason = "stop";
            break;
        }
        ++completion_tokens;
        std::string piece = llama_token_to_piece(slot->ctx_, id, false);
        if (params->stream) {
            char* p = append_http_response_message(obuf.p, 200);
            choice["delta"].setObject();
            choice["delta"]["content"].setString(piece);
            response->json["created"].setLong(timespec_real().tv_sec);
            response->content = make_event(response->json);
            choice.getObject().erase("delta");
            if (!send_response_chunk(response->content))
                return false;
        } else {
            response->content += piece;
        }
        if (!slot->eval_token(id)) {
            SLOG("ran out of context window");
            break;
        }
    }
    choice["finish_reason"].setString(finish_reason);

    // finalize response
    if (params->stream) {
        choice["delta"].setObject();
        choice["delta"]["content"].setString("");
        response->json["created"].setLong(timespec_real().tv_sec);
        response->content = make_event(response->json);
        choice.getObject().erase("delta");
        if (!send_response_chunk(response->content))
            return false;
        return send_response_finish();
    } else {
        Json& usage = response->json["usage"];
        usage.setObject();
        usage["prompt_tokens"].setLong(response->tokens.size());
        usage["completion_tokens"].setLong(completion_tokens);
        usage["total_tokens"].setLong(completion_tokens +
                                      response->tokens.size());
        choice["message"].setObject();
        choice["message"]["role"].setString("assistant");
        choice["message"]["content"].setString(std::move(response->content));
        response->json["created"].setLong(timespec_real().tv_sec);
        char* p = append_http_response_message(obuf.p, 200);
        p = stpcpy(p, "Content-Type: application/json\r\n");
        response->content = response->json.toString();
        return send_response(obuf.p, p, response->content);
    }
}