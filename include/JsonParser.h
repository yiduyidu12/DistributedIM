#pragma once

#include <string>

std::string getJsonString(const std::string& json, const std::string& key);
int getJsonInt(const std::string& json, const std::string& key);
std::string getJsonType(const std::string& json);