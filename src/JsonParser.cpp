#include "JsonParser.h"
#include <cctype>

std::string getJsonString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos += search.length();
    size_t end = json.find("\"", pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

int getJsonInt(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return 0;
    pos += search.length();
    while (pos < json.length() && json[pos] == ' ') pos++;
    size_t end = pos;
    while (end < json.length() && (isdigit(json[end]) || json[end] == '-')) end++;
    if (end == pos) return 0;
    return std::stoi(json.substr(pos, end - pos));
}

std::string getJsonType(const std::string& json) {
    return getJsonString(json, "type");
}