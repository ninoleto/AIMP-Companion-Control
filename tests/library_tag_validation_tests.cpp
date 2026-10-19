#include "library_tag_catalog.h"
#include <cassert>
#include <iostream>
#include <limits>
#include <set>

int main() {
    using namespace bridge_tags;
    using Json = nlohmann::json;
    std::set<std::string> ids;
    for (const auto& field : Fields) assert(ids.insert(field.id).second);
    // An editable file is never saved from an unconfirmed request, malformed
    // values, an arbitrary property ID, or a stale/missing file-identity token.
    assert(ValidateWrite({{"fields", {{"title", "New"}}}}) == "TAG_WRITE_CONFIRMATION_REQUIRED");
    assert(ValidateWrite({{"confirm", 1}, {"fields", {{"title", "New"}}}}) == "TAG_WRITE_CONFIRMATION_REQUIRED");
    assert(ValidateWrite({{"confirm", true}, {"fields", {{"title", "New"}}}}) == "TAG_FILE_IDENTITY_REQUIRED");
    Json request = {{"confirm", true}, {"expected_file_path", "D:\\Music\\Track.flac"}, {"fields", {{"title", "New"}, {"rating", 5}}}};
    assert(ValidateWrite(request).empty());
    request["fields"] = {{"title", ""}, {"lyrics", "First line\nSecond line"}};
    assert(ValidateWrite(request).empty());
    request["fields"] = {{"filename", "D:\\other"}};
    assert(ValidateWrite(request) == "UNSUPPORTED_TAG_FIELD");
    request["fields"] = {{"property", 18}, {"value", "other"}};
    assert(ValidateWrite(request) == "UNSUPPORTED_TAG_FIELD");
    request["fields"] = {{"title", std::string("safe\0hidden", 11)}};
    assert(ValidateWrite(request) == "INVALID_TAG_VALUE");
    for (const Json& bad : {Json(true), Json("5"), Json(5.1), Json(-1), Json(6), Json(std::numeric_limits<uint64_t>::max())}) {
        request["fields"] = {{"rating", bad}};
        assert(ValidateWrite(request) == "INVALID_TAG_VALUE");
    }
    request["fields"] = {{"track_gain", std::numeric_limits<double>::infinity()}};
    assert(ValidateWrite(request) == "INVALID_TAG_VALUE");
    request["fields"] = {{"title", std::string(4097, 'x')}};
    assert(ValidateWrite(request) == "INVALID_TAG_VALUE");
    request["fields"] = {{"lyrics", std::string(32768, 'x')}, {"comment", std::string(8192, 'x')}, {"url", std::string(8192, 'x')}, {"title", "x"}};
    assert(ValidateWrite(request) == "TAG_FIELDS_TOO_LARGE");
    request["fields"] = Json::object();
    assert(ValidateWrite(request) == "INVALID_TAG_FIELDS");
    request["fields"] = {{"title", "Test"}};
    request["expected_file_path"] = std::string("D:\\Music\\\0Track", 15);
    assert(ValidateWrite(request) == "TAG_FILE_IDENTITY_REQUIRED");
    std::cout << "Tag write confirmation, identity, exact-field allowlist, types and input bounds passed\n";
}
