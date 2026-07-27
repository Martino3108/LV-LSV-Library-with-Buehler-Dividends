/**
 * @file benchmark_log.h
 * @brief Mirror @c std::cout to a log file for benchmark / verify runs.
 */

#ifndef BENCHMARK_LOG_H
#define BENCHMARK_LOG_H

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <streambuf>
#include <string>

/** Mirror @c std::cout to a log file for Python plot scripts. */
class TeeBuf : public std::streambuf {
public:
    TeeBuf(std::streambuf* primary, std::streambuf* secondary);

protected:
    int overflow(int c) override;
    int sync() override;

private:
    std::streambuf* primary_;
    std::streambuf* secondary_;
};

class ScopedTeeLog {
public:
    explicit ScopedTeeLog(const char* path);
    ~ScopedTeeLog();

    ScopedTeeLog(const ScopedTeeLog&) = delete;
    ScopedTeeLog& operator=(const ScopedTeeLog&) = delete;

private:
    std::ofstream file_;
    TeeBuf teeBuf_;
    std::streambuf* oldCout_ = nullptr;
};

#endif
