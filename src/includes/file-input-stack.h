#ifndef DINIT_FILE_INPUT_STACK_H_INCLUDED
#define DINIT_FILE_INPUT_STACK_H_INCLUDED

#include <fstream>
#include <stack>
#include <string>
#include <utility>

#include "dinit-iostream.h"

// A stack of input sources (open files) for a service description
class file_input_stack
{
    struct input_file {
        dio::istream stream;
        std::string file_name;
        int line_num;
    };

    std::stack<input_file> input_stack;

public:
    file_input_stack() = default;
    file_input_stack(const file_input_stack &) = delete;

    // Push a new input file
    // TODO limit depth and/or prevent recursion
    void push(std::string file_name, dio::istream &&file)
    {
        input_stack.emplace(input_file {std::move(file), std::move(file_name), 0});
    }

    bool getline(std::string &rline)
    {
        while (!input_stack.empty()) {
            input_file &top = input_stack.top();
            top.line_num++;

            if (top.stream.get_line_until_eof_nx(rline)) {
                return true;
            }

            top.stream.throw_exception_on(dio::buffer_fail_bit | dio::input_fail_bit | dio::io_fail_bit);
            input_stack.pop();
        }
        return false;
    }

    // Get a line from the current file, without falling back to another file
    bool getline_same_file(std::string &rline)
    {
        if (!input_stack.empty()) {
            input_file &top = input_stack.top();

            if (top.stream.get_line_until_eof_nx(rline)) {
                top.line_num++;
                return true;
            }

            top.stream.throw_exception_on(dio::buffer_fail_bit | dio::input_fail_bit | dio::io_fail_bit);
        }
        return false;
    }

    // Note: current line is only correct after reading a line, and refers to the line just read
    unsigned current_line() const
    {
        return input_stack.top().line_num;
    }

    const std::string &current_file_name() const
    {
        return input_stack.top().file_name;
    }
};


#endif /* DINIT_FILE_INPUT_STACK_H_INCLUDED */
