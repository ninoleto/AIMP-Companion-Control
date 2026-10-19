#pragma once
#include "json.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace music_library {
using Json = nlohmann::json;
constexpr int MaxPage = 100;
constexpr int MaxOffset = 1000000;
constexpr int MaxFields = 16;
constexpr int MaxFilters = 8;
constexpr int MaxSorts = 3;
constexpr int MaxStorages = 64;
constexpr int MaxSchema = 256;
constexpr int MaxCachedPages = 16;
constexpr int MaxLoadedTracks = 10000;

struct Field { std::string id; std::string type; bool filterable = false; bool internal = false; };
struct Request {
    std::string storage, cursor, search;
    int offset = 0, limit = 50;
    bool any = false;
    std::vector<std::string> fields;
    Json filters = Json::array(), sort = Json::array();
};
inline bool SafeText(const Json& value, size_t maximum, bool allowEmpty = false) {
    if (!value.is_string()) return false;
    const auto& text = value.get_ref<const std::string&>();
    return (allowEmpty || !text.empty()) && text.size() <= maximum && text.find('\0') == std::string::npos;
}
inline bool Integer(const Json& value, int minimum, int maximum) {
    return value.is_number_integer() && value.get<double>() >= minimum && value.get<double>() <= maximum;
}
inline std::string ParseRequest(const Json& input, Request& out) {
    if (!input.is_object()) return "INVALID_QUERY";
    if (input.contains("cursor")) {
        if (!SafeText(input["cursor"], 80)) return "INVALID_CURSOR";
        for (const char* key : {"storage_id", "search", "filters", "sort", "offset", "limit", "fields", "match"})
            if (input.contains(key)) return "CURSOR_QUERY_CANNOT_CHANGE_FILTERS";
        out.cursor = input["cursor"].get<std::string>();
        return {};
    }
    if (!input.contains("storage_id") || !SafeText(input["storage_id"], 512)) return "INVALID_STORAGE_ID";
    out.storage = input["storage_id"].get<std::string>();
    if (input.contains("offset")) {
        if (!Integer(input["offset"], 0, MaxOffset)) return "INVALID_QUERY_OFFSET";
        out.offset = input["offset"].get<int>();
    }
    if (input.contains("limit")) {
        if (!Integer(input["limit"], 1, MaxPage)) return "INVALID_QUERY_LIMIT";
        out.limit = input["limit"].get<int>();
    }
    if (input.contains("search")) {
        if (!SafeText(input["search"], 512, true)) return "INVALID_SEARCH";
        out.search = input["search"].get<std::string>();
    }
    if (input.contains("match")) {
        if (!input["match"].is_string() || (input["match"] != "all" && input["match"] != "any")) return "INVALID_FILTER_MATCH";
        out.any = input["match"] == "any";
    }
    if (input.contains("fields")) {
        if (!input["fields"].is_array() || input["fields"].empty() || input["fields"].size() > MaxFields - 2) return "INVALID_FIELDS";
        std::set<std::string> seen;
        for (const auto& field : input["fields"]) {
            if (!SafeText(field, 128)) return "INVALID_FIELDS";
            auto name = field.get<std::string>();
            if (!seen.insert(name).second) return "DUPLICATE_FIELD";
            out.fields.push_back(std::move(name));
        }
    }
    if (input.contains("filters")) {
        if (!input["filters"].is_array() || input["filters"].size() > MaxFilters) return "INVALID_FILTERS";
        const std::set<std::string> operations{"eq", "ne", "contains", "starts_with", "ends_with", "lt", "lte", "gt", "gte", "between"};
        for (const auto& f : input["filters"]) {
            if (!f.is_object() || !f.contains("field") || !SafeText(f["field"], 128) || !f.contains("operator") ||
                !f["operator"].is_string() || !operations.count(f["operator"].get<std::string>()) || !f.contains("value")) return "INVALID_FILTER";
            if (f["operator"] == "between" && !f.contains("value2")) return "INVALID_FILTER";
        }
        out.filters = input["filters"];
    }
    if (input.contains("sort")) {
        if (!input["sort"].is_array() || input["sort"].size() > MaxSorts) return "INVALID_SORT";
        std::set<std::string> seen;
        for (const auto& item : input["sort"]) {
            if (!item.is_object() || !item.contains("field") || !SafeText(item["field"], 128) || !item.contains("direction") ||
                (item["direction"] != "asc" && item["direction"] != "desc")) return "INVALID_SORT";
            if (!seen.insert(item["field"].get<std::string>()).second) return "DUPLICATE_SORT_FIELD";
        }
        out.sort = input["sort"];
    }
    return {};
}
inline const Field* FindField(const std::vector<Field>& schema, const std::string& id) {
    for (const auto& field : schema) if (field.id == id) return &field;
    return nullptr;
}
inline int RowIndex(const std::string& query, const Json& rowId, const Json& rows) {
    if (!SafeText(rowId, 100) || !rows.is_array()) return -1;
    const auto id = rowId.get<std::string>();
    if (id.compare(0, query.size() + 1, query + ":") != 0) return -1;
    for (size_t i = 0; i < rows.size() && i < MaxPage; ++i)
        if (rows[i].is_object() && rows[i].contains("row_id") && rows[i]["row_id"] == id) return static_cast<int>(i);
    return -1;
}
inline bool IsString(const Field& field) { return field.type == "string" || field.type == "string_set" || field.type == "filename"; }
inline bool ValidValue(const Field& field, const Json& value) {
    if (IsString(field)) return SafeText(value, 1024, true);
    if (!value.is_number() || !std::isfinite(value.get<double>())) return false;
    if (field.type == "int32") return Integer(value, INT32_MIN, INT32_MAX);
    // JSON numbers beyond JavaScript's exact-integer range must not silently select the wrong record.
    if (field.type == "int64" || field.type == "filesize")
        return value.is_number_integer() && value.get<double>() >= -9007199254740991.0 && value.get<double>() <= 9007199254740991.0;
    return field.type == "number" || field.type == "datetime" || field.type == "duration";
}
inline std::string ValidateSchema(const Request& request, const std::vector<Field>& schema) {
    for (const auto& field : request.fields) if (!FindField(schema, field)) return "UNKNOWN_FIELD";
    for (const auto& sort : request.sort) if (!FindField(schema, sort["field"].get<std::string>())) return "UNKNOWN_SORT_FIELD";
    for (const auto& filter : request.filters) {
        const auto* field = FindField(schema, filter["field"].get<std::string>());
        if (!field || !field->filterable) return "FIELD_NOT_FILTERABLE";
        const auto operation = filter["operator"].get<std::string>();
        if (!IsString(*field) && (operation == "contains" || operation == "starts_with" || operation == "ends_with")) return "INVALID_FILTER_OPERATOR_FOR_TYPE";
        if (!ValidValue(*field, filter["value"]) || (operation == "between" && !ValidValue(*field, filter["value2"]))) return "INVALID_FILTER_VALUE";
        if (operation == "between" && !IsString(*field) && filter["value"].get<double>() > filter["value2"].get<double>()) return "INVALID_FILTER_RANGE";
    }
    return {};
}
inline std::vector<Field> LocalSchema() {
    // Official stable SDK AIMPML_LOCALDATASTORAGE_FIELD_* catalog. UserMark's
    // Double type follows IAIMPMLExtensionDataStorage CHM, correcting its header comment.
    std::vector<Field> fields;
    for (const char* id : {"ID", "BitDepth", "Bitrate", "BPM", "Channels", "PlaybackCount", "Rating", "SampleRate"}) fields.push_back({id, "int32", true});
    for (const char* id : {"Added", "LastModification", "LastPlayback"}) fields.push_back({id, "datetime", true});
    fields.push_back({"Duration", "duration", true}); fields.push_back({"FileSize", "filesize", true});
    fields.push_back({"FileName", "filename", true}); fields.push_back({"UserMark", "number", true});
    for (const char* id : {"Album", "AlbumArtist", "Artist", "Composer", "Genre", "Labels", "Lyricist", "Mood", "Publisher"}) fields.push_back({id, "string_set", true});
    for (const char* id : {"Comment", "Conductor", "Copyrights", "DiskNumber", "FileFormat", "Title", "TrackNumber", "URL", "Year"}) fields.push_back({id, "string", true});
    return fields;
}
}
