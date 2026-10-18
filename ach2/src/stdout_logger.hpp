#pragma once

#include <string>
#include <string_view>

namespace linear_service::logging {

// Сериализуем записи, чтобы строки параллельных асинхронных callback не перемешивались.
void write(const std::string& line);

// Экранируем управляющие символы и кавычки, чтобы одно событие оставалось одной строкой.
std::string escapeField(std::string_view value);

}  // namespace linear_service::logging
