#pragma once
#include "json.hpp"
#include <cmath>
#include <string>

namespace bridge_tags {
struct Field { const char* id; const char* label; int property; const char* type; double minimum; double maximum; size_t maxLength; };
// Property numbers are the stable AIMP_FILEINFO_PROPID_* values. Keeping this
// schema platform-independent lets the exact file-write validation run in tests.
inline constexpr Field Fields[] = {
    {"title","Title",25,"string",0,0,4096}, {"artist","Artist",6,"string",0,0,4096},
    {"album","Album",1,"string",0,0,4096}, {"album_artist","Album artist",3,"string",0,0,4096},
    {"genre","Genre",20,"string",0,0,4096}, {"date","Date / year",14,"string",0,0,256},
    {"track_number","Track number",27,"string",0,0,128}, {"track_total","Track total",29,"string",0,0,128},
    {"disc_number","Disc number",15,"string",0,0,128}, {"disc_total","Disc total",16,"string",0,0,128},
    {"comment","Comment",10,"string",0,0,8192}, {"lyrics","Lyrics",21,"string",0,0,32768},
    {"composer","Composer",11,"string",0,0,4096}, {"copyright","Copyright",12,"string",0,0,4096},
    {"publisher","Publisher",23,"string",0,0,4096}, {"conductor","Conductor",33,"string",0,0,4096},
    {"lyricist","Lyricist",37,"string",0,0,4096}, {"encoded_by","Encoded by",38,"string",0,0,4096},
    {"mood","Mood",34,"string",0,0,4096}, {"catalog","Catalog",35,"string",0,0,4096},
    {"isrc","ISRC",36,"string",0,0,256}, {"key","Musical key",46,"string",0,0,256},
    {"url","URL",30,"string",0,0,8192}, {"bpm","BPM",8,"integer",0,1000000,0},
    {"rating","File tag rating",39,"integer",0,5,0},
    {"track_gain","Track ReplayGain",26,"number",-1000,1000,0},
    {"album_gain","Album ReplayGain",4,"number",-1000,1000,0},
    {"track_peak","Track ReplayGain peak",28,"number",0,1000000,0},
    {"album_peak","Album ReplayGain peak",5,"number",0,1000000,0}
};
inline const Field* Find(const std::string& id) { for (const auto& field : Fields) if (id == field.id) return &field; return nullptr; }
inline std::string ValidateFields(const nlohmann::json& fields) {
    if (!fields.is_object() || fields.empty() || fields.size() > sizeof(Fields)/sizeof(Fields[0])) return "INVALID_TAG_FIELDS";
    size_t total = 0;
    for (auto entry = fields.begin(); entry != fields.end(); ++entry) {
        const auto* field = Find(entry.key());
        if (!field) return "UNSUPPORTED_TAG_FIELD";
        const auto& value = entry.value();
        if (std::string(field->type) == "string") {
            if (!value.is_string()) return "INVALID_TAG_VALUE";
            const auto& text = value.get_ref<const std::string&>();
            if (text.size() > field->maxLength || text.find('\0') != std::string::npos) return "INVALID_TAG_VALUE";
            total += text.size();
            if (total > 49152) return "TAG_FIELDS_TOO_LARGE";
        } else {
            if (!value.is_number() || (std::string(field->type) == "integer" && !value.is_number_integer())) return "INVALID_TAG_VALUE";
            const double number = value.get<double>();
            if (!std::isfinite(number) || number < field->minimum || number > field->maximum) return "INVALID_TAG_VALUE";
        }
    }
    return {};
}
inline std::string ValidateWrite(const nlohmann::json& command) {
    if (!command.contains("confirm") || !command["confirm"].is_boolean() || !command["confirm"].get<bool>()) return "TAG_WRITE_CONFIRMATION_REQUIRED";
    if (!command.contains("expected_file_path") || !command["expected_file_path"].is_string() ||
        command["expected_file_path"].get_ref<const std::string&>().empty() ||
        command["expected_file_path"].get_ref<const std::string&>().size() > 16384 ||
        command["expected_file_path"].get_ref<const std::string&>().find('\0') != std::string::npos) return "TAG_FILE_IDENTITY_REQUIRED";
    if (!command.contains("fields")) return "INVALID_TAG_FIELDS";
    return ValidateFields(command["fields"]);
}
}
