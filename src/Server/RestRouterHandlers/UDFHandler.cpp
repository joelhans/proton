#include "UDFHandler.h"
#include "SchemaValidator.h"

#include <Interpreters/Streaming/ASTToJSONUtils.h>
#include <Interpreters/Streaming/MetaStoreJSONConfigRepository.h>
#include <V8/Utils.h>

#include <numeric>

namespace DB
{
namespace ErrorCodes
{
extern const int BAD_REQUEST_PARAMETER;
extern const int UDF_INVALID_NAME;
}

namespace
{
String buildResponse(const String & query_id)
{
    Poco::JSON::Object resp;
    resp.set("request_id", query_id);

    std::stringstream resp_str_stream; /// STYLE_CHECK_ALLOW_STD_STRING_STREAM
    resp.stringify(resp_str_stream, 0);
    return resp_str_stream.str();
}

String buildResponse(const Poco::JSON::Array::Ptr & object, const String & query_id)
{
    Poco::JSON::Object resp;
    resp.set("request_id", query_id);
    resp.set("data", object);
    std::stringstream resp_str_stream; /// STYLE_CHECK_ALLOW_STD_STRING_STREAM
    resp.stringify(resp_str_stream, 0);
    return resp_str_stream.str();
}

String buildResponse(const Poco::JSON::Object::Ptr & object, const String & query_id)
{
    Poco::JSON::Object resp;
    resp.set("request_id", query_id);
    resp.set("data", object);
    std::stringstream resp_str_stream; /// STYLE_CHECK_ALLOW_STD_STRING_STREAM
    resp.stringify(resp_str_stream, 0);
    return resp_str_stream.str();
}

/// FIXME, refactor out to a util since SQL needs this validation as well
/// Valid UDF function name is [a-zA-Z0-9_] and has to start with _ or [a-zA-Z]
void validateUDFName(const String & func_name)
{
    if (func_name.empty())
        throw Exception(ErrorCodes::UDF_INVALID_NAME, "UDF name must not be empy");

    /// At least one alphabetic
    bool has_alphabetic = false;
    for (auto ch : func_name)
    {
        if(!std::isalnum(ch) && ch != '_')
            throw Exception(ErrorCodes::UDF_INVALID_NAME, "UDF name can contain only alphabetic, numbers and underscores");

        has_alphabetic = std::isalpha(ch);
    }

    if (!has_alphabetic)
        throw Exception(ErrorCodes::UDF_INVALID_NAME, "UDF name shall contain at lest one alphabetic");

    if (!std::isalpha(func_name[0]) && func_name[0] != '_')
        throw Exception(ErrorCodes::UDF_INVALID_NAME, "UDF name's first char shall be an alphabetic or underscore");
}
}

std::pair<String, Int32> UDFHandler::executeGet(const Poco::JSON::Object::Ptr & /*payload*/) const
{
    const auto & func = getPathParameter("func");
    if (func.empty())
    {
        /// List all functions
        Poco::JSON::Array::Ptr functions = metastore_repo->list();
        return {buildResponse(functions, query_context->getCurrentQueryId()), HTTPResponse::HTTP_OK};
    }

    /// Get by function name
    Poco::JSON::Object::Ptr object = metastore_repo->get(func);
    Poco::JSON::Object::Ptr def = object->getObject("function");
    return {buildResponse(def, query_context->getCurrentQueryId()), HTTPResponse::HTTP_OK};
}

std::map<String, std::map<String, String>> UDFHandler::create_schema
    = {{"required", {{"name", "string"}, {"type", "string"}, {"return_type", "string"}}},
       {"optional", {{"format", "string"}, {"command", "string"}, {"url", "string"}, {"auth_method", "string"}}}};

bool UDFHandler::validatePost(const Poco::JSON::Object::Ptr & payload, String & error_msg) const
{
    if (!validateSchema(create_schema, payload, error_msg))
        return false;

    /// Validate type
    auto type = payload->get("type").toString();
    if (type == "remote")
    {
        if (!payload->has("url"))
        {
            error_msg = "Missing 'url' property for 'remote' UDF";
            return false;
        }
        auto url = payload->get("url").toString();
        Poco::URI uri;
        try
        {
            uri = url;
        }
        catch (const Poco::SyntaxException & e)
        {
            error_msg = fmt::format("Invalid url for remote UDF, msg: {}", e.message());
            return false;
        }
    }
    else if (type == "executable")
    {
        if (!payload->has("command"))
        {
            error_msg = "Missing 'command' property for 'executable' UDF";
            return false;
        }
    }
    else if (type == "javascript")
    {
        if (!payload->has("source"))
        {
            error_msg = "Missing 'source' property for 'javascript' UDF";
            return false;
        }

        const auto & func_name = payload->get("name").toString();
        validateUDFName(func_name);

        /// check whether the source can be compiled
        if (payload->has("is_aggregation") && payload->getValue<bool>("is_aggregation"))
            /// UDA
            V8::validateAggregationFunctionSource(func_name, {"initialize", "process", "finalize"}, payload->get("source"));
        else
            /// UDF
            V8::validateStatelessFunctionSource(func_name, payload->get("source"));
    }
    else
    {
        error_msg = fmt::format("Invalid type {}, only support 'remote', 'executable' or 'javascript'", type);
        return false;
    }

    /// Validate arguments
    if (!payload->has("arguments"))
    {
        error_msg = "Missing argument definition.";
        return false;
    }

    const auto & args = payload->getArray("arguments");
    if (args->size() < 1)
    {
        error_msg = fmt::format("Empty input argument is not supported.");
        return false;
    }

    for (unsigned int i = 0; i < args->size(); i++)
    {
        if (!args->getObject(i))
        {
            error_msg = "Invalid argument, `arguments` are expected to contain 'name' and 'type' information.";
            return false;
        }

        if (!args->getObject(i)->has("name") || !args->getObject(i)->has("type"))
        {
            error_msg = "Invalid argument, missing argument 'name or 'type'.";
            return false;
        }
    }

    /// Validate auth_method
    if (payload->has("auth_method"))
    {
        if (!payload->has("auth_context"))
        {
            error_msg = "Missing 'auth_context'";
            return false;
        }
        String method = payload->get("auth_method").toString();
        auto ctx = payload->getObject("auth_context");
        if (method == "auth_header")
        {
            if (!ctx->has("key_name") && !ctx->has("key_value"))
            {
                error_msg = "'auth_header' auth_method requires 'key_name' and 'key_value' in 'auth_context'";
                return false;
            }
        }
    }

    return true;
}

std::pair<String, Int32> UDFHandler::executePost(const Poco::JSON::Object::Ptr & payload) const
{
    String name = payload->get("name").toString();
    Poco::JSON::Object::Ptr func(new Poco::JSON::Object());
    func->set("function", payload);
    metastore_repo->save(name, func);
    return {buildResponse(query_context->getCurrentQueryId()), HTTPResponse::HTTP_OK};
}

std::pair<String, Int32> UDFHandler::executeDelete(const Poco::JSON::Object::Ptr & /*payload*/) const
{
    const auto & func = getPathParameter("func", "");
    if (func.empty())
        return {jsonErrorResponse("Missing UDF name", ErrorCodes::BAD_REQUEST_PARAMETER), HTTPResponse::HTTP_BAD_REQUEST};

    metastore_repo->remove(func);
    return {buildResponse(query_context->getCurrentQueryId()), HTTPResponse::HTTP_OK};
}
}
