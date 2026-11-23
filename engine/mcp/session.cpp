// THE MCP SESSION (MCP-ON-HOST-1 stage 2): mcp/include/mcp/session.h says the laws. The protocol's facts:
// spikes/mcp-on-host/PROTOCOL.md (revision 2026-07-28).
#include "mcp/session.h"

#include "mcp/schema.h"

#include <cctype>
#include <utility>

namespace jahshaka { namespace engine { namespace mcp {

using script::Access;
using script::CommandInfo;
using script::Host;
using script::Kind;
using script::Result;
using script::Status;
using script::Value;

namespace {

constexpr int kParseError = -32700, kInvalidRequest = -32600, kMethodNotFound = -32601, kInvalidParams = -32602,
              kInternalError = -32603, kUnsupportedVersion = -32022;
constexpr double kProgressEvery = 30;   // at most one progress notification per 30 frames, plus the last
constexpr std::int64_t kListTtlMs = 3600000;

Value object(Value::Object o) { return Value(std::move(o)); }
Value strings(const std::vector<std::string> &s)
{
    Value::Array a;
    for (const std::string &x : s) a.emplace_back(x);
    return Value(std::move(a));
}
std::string lower(std::string s)
{
    for (char &c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// A tool's annotations: a read-only tool, or the worst case (it may change and destroy; never the open world).
Value annotations(bool readOnly)
{
    if (readOnly) return object({{"readOnlyHint", Value(true)}, {"openWorldHint", Value(false)}});
    return object({{"readOnlyHint", Value(false)}, {"destructiveHint", Value(true)}, {"idempotentHint", Value(false)},
                   {"openWorldHint", Value(false)}});
}
Value stringProperty(const char *description)
{
    return object({{"type", Value("string")}, {"description", Value(description)}});
}
Value commandAndArgs(const char *what)
{
    return object({{"type", Value("object")},
                   {"properties", object({{"command", stringProperty("The command's full name, e.g. engine.frames.")},
                                          {"args", object({{"type", Value("object")},
                                                           {"description", Value(what)},
                                                           {"default", object({})}})}})},
                   {"required", strings({"command"})},
                   {"additionalProperties", Value(false)}});
}

/// THE FIVE TOOLS, in their fixed order (L4).
Value toolList()
{
    Value::Array tools;
    tools.push_back(object(
        {{"name", Value("api_docs")},
         {"title", Value("Engine API docs")},
         {"description", Value("List or search the engine's script commands: each match's signature, access, doc and the "
                               "JSON Schema of its arguments (for call and read). No argument: the index of command names. "
                               "Give ONE of command, search or root.")},
         {"inputSchema",
          object({{"type", Value("object")},
                  {"properties", object({{"command", stringProperty("One command by its exact name, e.g. engine.frames.")},
                                         {"search", stringProperty("Case-insensitive words matched against names and docs.")},
                                         {"root", stringProperty("Every command under a name prefix, e.g. engine.gi.")}})},
                  {"additionalProperties", Value(false)}})},
         {"annotations", annotations(true)}}));
    tools.push_back(object(
        {{"name", Value("call")},
         {"title", Value("Call one engine command")},
         {"description", Value("Run one registered engine command with its arguments BY PARAMETER NAME (see api_docs). "
                               "Answers {status, value, console} as JSON. Scenes, views and pictures are integer handles that "
                               "live until released or until this server process exits. A failed or refused command is an "
                               "error result carrying the engine's message.")},
         {"inputSchema", commandAndArgs("The command's arguments by parameter name, as api_docs gives them.")},
         {"annotations", annotations(false)}}));
    tools.push_back(object(
        {{"name", Value("read")},
         {"title", Value("Read from the engine")},
         {"description", Value("The same as call, for a command whose access is [reads] only (it changes nothing the engine "
                               "holds); any other command is an error result naming its access.")},
         {"inputSchema", commandAndArgs("The command's arguments by parameter name, as api_docs gives them.")},
         {"annotations", annotations(true)}}));
    tools.push_back(object(
        {{"name", Value("run_script")},
         {"title", Value("Run an engine script")},
         {"description", Value("Run a script on the engine's script host. Answers {ok, result | error, console} as JSON; "
                               "result is the script's completion value (its last expression; a promise is awaited). Each "
                               "call runs in a FRESH script realm: keep state in engine handles, not script globals. A "
                               "script has no clock (no Date) and Math.random is a fixed stream.")},
         {"inputSchema",
          object({{"type", Value("object")},
                  {"properties", object({{"script", stringProperty("The script source.")},
                                         {"language", object({{"type", Value("string")},
                                                              {"description", Value("The script's language (its file "
                                                                                    "extension without the dot).")},
                                                              {"default", Value("js")}})}})},
                  {"required", strings({"script"})},
                  {"additionalProperties", Value(false)}})},
         {"annotations", annotations(false)}}));
    tools.push_back(object(
        {{"name", Value("picture")},
         {"title", Value("See pictures")},
         {"description", Value("Return existing picture handles (from engine.view.readPixels) as PNG images, one per handle "
                               "(two eyes = two handles, in order). Draws no frame and waits for nothing.")},
         {"inputSchema",
          object({{"type", Value("object")},
                  {"properties", object({{"pictures", object({{"type", Value("array")},
                                                              {"items", object({{"type", Value("integer")},
                                                                                {"minimum", Value(1)}})},
                                                              {"minItems", Value(1)},
                                                              {"maxItems", Value(4)}})},
                                         {"maxEdge", object({{"type", Value("integer")},
                                                             {"minimum", Value(16)},
                                                             {"maximum", Value(2048)},
                                                             {"default", Value(1024)},
                                                             {"description", Value("Longest edge in pixels after a box "
                                                                                   "downscale.")}})}})},
                  {"required", strings({"pictures"})},
                  {"additionalProperties", Value(false)}})},
         {"annotations", annotations(true)}}));
    return Value(std::move(tools));
}

/// A command's entry in api_docs: its name, signature, access, doc and argument schema.
Value entry(const CommandInfo &c)
{
    Value::Object e{{"name", Value(c.name)},
                    {"signature", Value(script::signatureText(c))},
                    {"access", Value(script::accessName(c.signature.access))},
                    {"doc", Value(c.doc)}};
    Value schema;
    std::string why;
    if (argumentSchema(c, schema, why)) e.emplace_back("inputSchema", std::move(schema));
    else e.emplace_back("inputSchemaFault", Value(why));
    return Value(std::move(e));
}

/// A tool's members checked against the names it takes: an unknown one is named (an input error: isError).
bool onlyMembers(const Value &args, std::initializer_list<const char *> names, std::string &why)
{
    for (const Value::Member &m : args.asObject()) {
        bool known = false;
        for (const char *n : names) known = known || m.first == n;
        if (!known) {
            why = "no argument '" + m.first + "'";
            return false;
        }
    }
    return true;
}

const char *statusName(Status s) { return s == Status::Ok ? "ok" : s == Status::Refused ? "refused" : "failed"; }

}  // namespace

Session::Session(Doors doors) : mDoors(std::move(doors)) {}

Session::~Session() = default;

void Session::receive(Incoming in)
{
    if (mEnded) return;
    mQueue.push_back(std::move(in));
    if (mServing) return;
    mServing = true;
    while (!mQueue.empty() && !mEnded) {
        const Incoming next = std::move(mQueue.front());
        mQueue.pop_front();
        serve(next);
    }
    mServing = false;
}

void Session::end()
{
    mEnded = true;
    mQueue.clear();
    if (mEngine) mEngine->registry().releaseScoped();
}

void Session::serve(const Incoming &in)
{
    if (in.overlong)
        return error(nullptr, kParseError, "Parse error: a line over " + std::to_string(kLineCap) + " bytes (the cap)");
    Value m;
    std::string why;
    if (!iris::json::parse(in.line, m, why)) return error(nullptr, kParseError, "Parse error: " + why);
    if (m.kind() != Kind::Object) return error(nullptr, kInvalidRequest, "Invalid Request: a message is one JSON object");
    const Value *id = m.find("id");
    const bool idOk = id && (id->kind() == Kind::Int || id->kind() == Kind::String);
    const Value &method = m["method"];
    if (m["jsonrpc"] != Value("2.0") || method.kind() != Kind::String || (id && !idOk))
        return error(idOk ? id : nullptr, kInvalidRequest, "Invalid Request: not a JSON-RPC 2.0 request or notification");
    const Value &params = m["params"];
    if (!id) {   // a notification: never answered; an unknown one dropped
        if (method.asString() == "notifications/cancelled" && params.kind() == Kind::Object)
            cancelled(params["requestId"]);
        return;
    }
    request(*id, method.asString(), params);
}

void Session::request(const Value &id, const std::string &method, const Value &params)
{
    if (method == "initialize")
        return error(&id, kUnsupportedVersion,
                     std::string("Unsupported protocol version: this server speaks ") + kRevision + " only (no initialize)",
                     object({{"supported", strings({kRevision})},
                             {"requested", params.kind() == Kind::Object ? params["protocolVersion"] : Value()}}));
    if (method != "server/discover" && method != "tools/list" && method != "tools/call")
        return error(&id, kMethodNotFound, "Method not found: " + method);
    if (!params.isNull() && params.kind() != Kind::Object) return error(&id, kInvalidParams, "Invalid params: not an object");
    const Value &meta = params["_meta"];
    const Value &version = meta["io.modelcontextprotocol/protocolVersion"];
    if (version.kind() != Kind::String || meta["io.modelcontextprotocol/clientCapabilities"].kind() != Kind::Object)
        return error(&id, kInvalidParams,
                     "Invalid params: _meta must carry io.modelcontextprotocol/protocolVersion and clientCapabilities");
    if (version.asString() != kRevision)
        return error(&id, kUnsupportedVersion, "Unsupported protocol version",
                     object({{"supported", strings({kRevision})}, {"requested", version}}));

    if (method == "server/discover")
        return reply(id, object({{"supportedVersions", strings({kRevision})},
                                 {"capabilities", object({{"tools", object({})}})},
                                 {"instructions", Value("The Jahshaka engine, headless: read api_docs first; call commands by "
                                                        "parameter name; state lives in integer handles.")},
                                 {"ttlMs", Value(kListTtlMs)},
                                 {"cacheScope", Value("public")},
                                 {"_meta", object({{"io.modelcontextprotocol/serverInfo",
                                                    object({{"name", Value("jahengine")},
                                                            {"version", Value(mDoors.version)}})}})}}));
    if (method == "tools/list")
        return reply(id, object({{"tools", toolList()}, {"ttlMs", Value(kListTtlMs)}, {"cacheScope", Value("public")}}));

    // tools/call
    const Value &name = params["name"];
    const Value &args = params["arguments"];
    if (name.kind() != Kind::String || !(args.isNull() || args.kind() == Kind::Object))
        return error(&id, kInvalidParams, "Invalid params: tools/call takes a name and an arguments object");
    const std::string &tool = name.asString();
    if (tool != "api_docs" && tool != "call" && tool != "read" && tool != "run_script" && tool != "picture")
        return error(&id, kInvalidParams, "Unknown tool: " + tool);
    mRunning = &id;
    mCancelled = false;
    mProgressToken = meta["progressToken"];
    mProgressSent = 0;
    bool isError = false;
    Value structured = toolCall(tool, args.isNull() ? Value(Value::Object{}) : args, isError);
    mRunning = nullptr;
    mProgressToken = Value();
    if (mCancelled) {   // NO reply, no progress, for a cancelled request
        const Value &said = structured.find("message") ? structured["message"] : structured["error"];
        mDoors.log("cancelled: request " + (id.kind() == Kind::Int ? std::to_string(id.asInt()) : id.asString()) + " (" +
                   tool + ")" + (said.kind() == Kind::String ? ": " + said.asString() : std::string()));
        return;
    }
    std::string text, why;
    if (!iris::json::write(structured, text, why)) {
        isError = true;
        structured = object({{"error", Value("the answer cannot be carried: " + why)}});
        iris::json::write(structured, text, why);
    }
    Value::Object result{{"content", Value(Value::Array{object({{"type", Value("text")}, {"text", Value(text)}})})},
                         {"structuredContent", std::move(structured)}};
    if (isError) result.emplace_back("isError", Value(true));
    reply(id, Value(std::move(result)));
}

Host *Session::host(bool engine)
{
    if (engine && !mEngine) {   // THE LAZY BOOT (L11): the first engine-reaching tools/call makes the ONE engine host
        Host::Options o;
        o.freshRealmPerRun = true;
        o.completionBounds = kCompletionBounds;
        o.memoryLimit = kHeapLimit;
        o.outsideClient = true;
        mEngine = mDoors.makeHost(o, true);
        mDocs.reset();
        mEngine->setConsole([this](const std::string &line) {
            mDoors.log(line);
            if (mConsoleBytes + line.size() > kConsoleCap) {
                ++mConsoleLeftOut;
                return;
            }
            mConsoleBytes += line.size();
            mConsole.push_back(line);
        });
        mEngine->setKeepGoing([this](double done, double total) { return keepGoing(done, total); });
    }
    if (mEngine) return mEngine.get();
    if (!mDocs) {
        Host::Options o;
        o.documentationOnly = true;
        mDocs = mDoors.makeHost(o, false);
    }
    return mDocs.get();
}

Value Session::toolCall(const std::string &tool, const Value &args, bool &isError)
{
    std::string why;
    if (tool == "api_docs") return apiDocs(args, isError);
    const auto fail = [&isError](const std::string &message) {
        isError = true;
        return object({{"error", Value(message)}});
    };
    Host *h = host(true);
    mConsole.clear();
    mConsoleBytes = mConsoleLeftOut = 0;
    const auto withConsole = [this](Value::Object o) {
        if (mConsoleLeftOut) mConsole.push_back("(" + std::to_string(mConsoleLeftOut) + " more console lines on stderr)");
        if (!mConsole.empty()) o.emplace_back("console", strings(mConsole));
        return Value(std::move(o));
    };
    if (tool == "picture") return fail("picture: not yet (MCP-ON-HOST-1 stage 3)");
    if (tool == "run_script") {
        if (!onlyMembers(args, {"script", "language"}, why)) return fail("run_script: " + why);
        const Value &language = args["language"];
        if (args["script"].kind() != Kind::String || !(language.isNull() || language.kind() == Kind::String))
            return fail("run_script: script must be a string, language a string");
        const Result r = h->run(args["script"].asString(), "run_script." + (language.isNull() ? "js" : language.asString()));
        isError = r.status != Status::Ok;
        if (isError) return withConsole({{"ok", Value(false)}, {"error", Value(r.message)}});
        return withConsole({{"ok", Value(true)}, {"result", r.value}});
    }
    // call / read
    if (!onlyMembers(args, {"command", "args"}, why)) return fail(tool + ": " + why);
    const Value &command = args["command"];
    const Value &named = args["args"];
    if (command.kind() != Kind::String || !(named.isNull() || named.kind() == Kind::Object))
        return fail(tool + ": command must be a string, args an object");
    if (tool == "read") {
        const CommandInfo *c = h->registry().find(command.asString());
        if (c && c->signature.access != Access::Reads)
            return fail("read: " + c->name + " is not a reads command (its access: " +
                        script::accessName(c->signature.access) + "): use call");
    }
    const Result r = h->callNamed(command.asString(), named.isNull() ? Value::Object{} : named.asObject());
    isError = r.status != Status::Ok;
    Value::Object o{{"status", Value(statusName(r.status))}, {"value", r.value}};
    if (!r.message.empty()) o.emplace_back("message", Value(r.message));
    return withConsole(std::move(o));
}

Value Session::apiDocs(const Value &args, bool &isError)
{
    std::string why;
    const auto fail = [&isError](const std::string &message) {
        isError = true;
        return object({{"error", Value(message)}});
    };
    if (!onlyMembers(args, {"command", "search", "root"}, why)) return fail("api_docs: " + why);
    if (args.asObject().size() > 1) return fail("api_docs: give ONE of command, search or root");
    for (const Value::Member &m : args.asObject())
        if (m.second.kind() != Kind::String) return fail("api_docs: " + m.first + " must be a string");
    const script::Registry &reg = host(false)->registry();
    Value::Array found;
    if (args.asObject().empty()) {   // the index of names
        Value::Array names;
        for (const CommandInfo *c : reg.commands()) names.emplace_back(c->name);
        Value::Object o{{"commands", Value(std::move(names))}};
        if (reg.faulted()) o.emplace_back("faults", strings(reg.validate()));
        return Value(std::move(o));
    }
    const std::string &key = args.asObject()[0].first, &text = args.asObject()[0].second.asString();
    if (key == "command") {
        const CommandInfo *c = reg.find(text);
        if (!c) return fail("api_docs: no command '" + text + "' (no argument: the index of names)");
        found.push_back(entry(*c));
    } else {
        std::vector<std::string> words;
        for (std::size_t at = 0; key == "search" && at < text.size();) {
            const std::size_t end = text.find(' ', at);
            const std::string w = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (!w.empty()) words.push_back(lower(w));
            at = end == std::string::npos ? text.size() : end + 1;
        }
        for (const CommandInfo *c : reg.commands()) {
            bool match = key == "root" ? c->name.compare(0, text.size(), text) == 0 : !words.empty();
            const std::string hay = lower(c->name + " " + c->doc);
            for (const std::string &w : words) match = match && hay.find(w) != std::string::npos;
            if (match) found.push_back(entry(*c));
        }
    }
    return object({{"commands", Value(std::move(found))}});
}

bool Session::keepGoing(double done, double total)
{
    std::vector<Incoming> arrived;
    if (!mDoors.poll(arrived)) {   // stdin ended: cancel the running request, then end
        mCancelled = true;
        mEnded = true;
    }
    for (Incoming &in : arrived) {
        Value m;
        std::string why;
        if (!in.overlong && iris::json::parse(in.line, m, why) && m.kind() == Kind::Object && !m.find("id") &&
            m["method"] == Value("notifications/cancelled")) {
            cancelled(m["params"]["requestId"]);
            continue;
        }
        mQueue.push_back(std::move(in));
    }
    if (!mCancelled && !mProgressToken.isNull() && total > 0 && done > mProgressSent &&
        (done - mProgressSent >= kProgressEvery || done >= total)) {
        mProgressSent = done;
        write(object({{"jsonrpc", Value("2.0")},
                      {"method", Value("notifications/progress")},
                      {"params", object({{"progressToken", mProgressToken},
                                         {"progress", Value(std::int64_t(done))},
                                         {"total", Value(std::int64_t(total))},
                                         {"message", Value("frame " + std::to_string(std::int64_t(done)) + " of " +
                                                           std::to_string(std::int64_t(total)))}})}}));
    }
    return !mCancelled;
}

void Session::cancelled(const Value &requestId)
{
    if (mRunning && requestId == *mRunning && requestId.kind() == mRunning->kind()) {
        mCancelled = true;
        return;
    }
    for (auto it = mQueue.begin(); it != mQueue.end(); ++it) {   // a queued request: dropped unanswered
        Value m;
        std::string why;
        if (it->overlong || !iris::json::parse(it->line, m, why) || m.kind() != Kind::Object) continue;
        const Value *id = m.find("id");
        if (id && *id == requestId && id->kind() == requestId.kind()) {
            mQueue.erase(it);
            return;
        }
    }
}   // (after its reply, or an unknown id: ignored)

void Session::reply(const Value &id, Value result)
{
    Value::Object r = std::move(result.asObject());
    r.insert(r.begin(), Value::Member{"resultType", Value("complete")});
    write(object({{"jsonrpc", Value("2.0")}, {"id", id}, {"result", Value(std::move(r))}}));
}

void Session::error(const Value *id, int code, const std::string &message, Value data)
{
    Value::Object e{{"code", Value(code)}, {"message", Value(message)}};
    if (!data.isNull()) e.emplace_back("data", std::move(data));
    Value::Object m{{"jsonrpc", Value("2.0")}};
    if (id) m.emplace_back("id", *id);   // (no id when it could not be read: the MCP schema's optional id)
    m.emplace_back("error", Value(std::move(e)));
    write(Value(std::move(m)));
}

void Session::write(Value message)
{
    std::string text, why;
    if (iris::json::write(message, text, why)) return mDoors.send(text);
    // the message cannot be carried (invalid UTF-8 in a message of ours): an internal error, its id kept
    Value::Object m{{"jsonrpc", Value("2.0")}};
    if (const Value *id = message.find("id")) m.emplace_back("id", *id);
    m.emplace_back("error", object({{"code", Value(kInternalError)},
                                    {"message", Value("Internal error: the reply cannot be carried")}}));
    if (iris::json::write(Value(std::move(m)), text, why)) mDoors.send(text);
}

}}}  // namespace jahshaka::engine::mcp
