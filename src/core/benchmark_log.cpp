/**
 * @file benchmark_log.cpp
 */

#include "benchmark_log.h"

TeeBuf::TeeBuf(std::streambuf* primary, std::streambuf* secondary)
    : primary_(primary), secondary_(secondary) {}

int TeeBuf::overflow(int c) {
    if (traits_type::eq_int_type(c, traits_type::eof())) {
        return traits_type::not_eof(c);
    }
    const char ch = traits_type::to_char_type(c);
    if (primary_->sputc(ch) == traits_type::eof() || secondary_->sputc(ch) == traits_type::eof()) {
        return traits_type::eof();
    }
    return traits_type::not_eof(c);
}

int TeeBuf::sync() {
    const int r1 = primary_->pubsync();
    const int r2 = secondary_->pubsync();
    return (r1 == 0 && r2 == 0) ? 0 : -1;
}

ScopedTeeLog::ScopedTeeLog(const char* path)
    : file_(path, std::ios::out | std::ios::trunc), teeBuf_(std::cout.rdbuf(), file_.rdbuf()) {
    if (!file_.good()) {
        throw std::runtime_error(std::string("Cannot open log file: ") + path);
    }
    oldCout_ = std::cout.rdbuf(&teeBuf_);
}

ScopedTeeLog::~ScopedTeeLog() {
    if (oldCout_ != nullptr) {
        std::cout.rdbuf(oldCout_);
    }
    if (file_.is_open()) {
        file_.flush();
    }
}
