#include "music_library_validation.h"
#include <cassert>
#include <iostream>
#include <limits>

using namespace music_library;
std::string Validate(Json command, const std::vector<Field>& schema = LocalSchema()) {
    Request request;
    auto error = ParseRequest(command, request);
    return error.empty() ? ValidateSchema(request, schema) : error;
}
int main() {
    Json q = {{"storage_id", "TAIMPMLLocalDataStorage"}, {"limit", 100}, {"offset", 1000000},
        {"search", "Bjork"}, {"filters", {{{"field", "Duration"}, {"operator", "between"}, {"value", 60}, {"value2", 240}}}},
        {"sort", {{{"field", "Artist"}, {"direction", "asc"}}, {{"field", "Title"}, {"direction", "desc"}}}}};
    assert(Validate(q).empty());
    for (const auto& bad : {Json(0), Json(-1), Json(101), Json(true), Json("10"), Json(5.5), Json(std::numeric_limits<uint64_t>::max())}) {
        auto input = q; input["limit"] = bad; assert(Validate(input) == "INVALID_QUERY_LIMIT");
    }
    for (const auto& bad : {Json(-1), Json(1000001), Json(true), Json(0.1), Json(std::numeric_limits<uint64_t>::max())}) {
        auto input = q; input["offset"] = bad; assert(Validate(input) == "INVALID_QUERY_OFFSET");
    }
    auto input = q; input["storage_id"] = std::string("local\0suffix", 12);
    assert(Validate(input) == "INVALID_STORAGE_ID");
    input = q; input["search"] = std::string(513, 'a'); assert(Validate(input) == "INVALID_SEARCH");
    input = q; input["fields"] = {"Title", "Title"}; assert(Validate(input) == "DUPLICATE_FIELD");
    input = q; input["fields"] = {"UntrustedProperty999"}; assert(Validate(input) == "UNKNOWN_FIELD");
    input = q; input["sort"] = {{{"field", "Title"}, {"direction", "up"}}}; assert(Validate(input) == "INVALID_SORT");
    input["sort"] = {{{"field", "Title"}, {"direction", "asc"}}, {{"field", "Title"}, {"direction", "desc"}}};
    assert(Validate(input) == "DUPLICATE_SORT_FIELD");
    input = q; input["filters"][0]["field"] = "Title"; assert(Validate(input) == "INVALID_FILTER_VALUE");
    input = q; input["filters"][0]["operator"] = "contains"; assert(Validate(input) == "INVALID_FILTER_OPERATOR_FOR_TYPE");
    input = q; input["filters"][0]["value"] = 300; assert(Validate(input) == "INVALID_FILTER_RANGE");
    input = q; input["filters"][0]["value"] = std::numeric_limits<double>::infinity(); assert(Validate(input) == "INVALID_FILTER_VALUE");
    input = q; input["filters"] = {{{"field", "ID"}, {"operator", "eq"}, {"value", 2147483648LL}}}; assert(Validate(input) == "INVALID_FILTER_VALUE");
    input["filters"][0]["value"] = 1; assert(Validate(input).empty());
    input["filters"][0]["field"] = "FileSize";
    input["filters"][0]["value"] = 9007199254740991LL; assert(Validate(input).empty());
    input["filters"][0]["value"] = 9007199254740992LL; assert(Validate(input) == "INVALID_FILTER_VALUE");
    input["filters"][0]["value"] = "9007199254740991"; assert(Validate(input) == "INVALID_FILTER_VALUE");
    input = q; input["filters"] = Json::array();
    for (int i = 0; i < 9; ++i) input["filters"].push_back({{"field", "ID"}, {"operator", "eq"}, {"value", i}});
    assert(Validate(input) == "INVALID_FILTERS");
    input = {{"storage_id", "custom"}, {"filters", {{{"field", "private"}, {"operator", "eq"}, {"value", "x"}}}}};
    assert(Validate(input, {{"private", "string", false, true}}) == "FIELD_NOT_FILTERABLE");
    input = {{"cursor", "ml-1:next"}};
    Request cursor; assert(ParseRequest(input, cursor).empty());
    for (const char* changed : {"storage_id", "limit", "offset", "sort", "search", "filters", "fields", "match"}) {
        auto modified = input; modified[changed] = nullptr; Request result;
        assert(ParseRequest(modified, result) == "CURSOR_QUERY_CANNOT_CHANGE_FILTERS");
    }
    // Cached-row identity is query-scoped: a stale preset must never target the
    // same index in a newer page or accept a caller-supplied record ID/path.
    Json rows = {{{"row_id", "ml-7:0"}}, {{"row_id", "ml-7:1"}}};
    assert(RowIndex("ml-7", "ml-7:1", rows) == 1);
    assert(RowIndex("ml-8", "ml-7:1", rows) == -1);
    assert(RowIndex("ml-7", "ml-7:9", rows) == -1);
    assert(RowIndex("ml-7", "D:\\Music\\arbitrary.mp3", rows) == -1);
    assert(RowIndex("ml-7", 1, rows) == -1);
    std::cout << "Music-library typed filters, query bounds, cursor isolation, and stale-row guards passed\n";
}
