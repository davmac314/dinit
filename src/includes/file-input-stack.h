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
        int parent_dir_fd;

        input_file(dio::istream &&stream_p, std::string &&file_name_p, int line_num_p, int parent_dir_fd_p) :
                stream(std::move(stream_p)), file_name(std::move(file_name_p)), line_num(line_num_p),
                parent_dir_fd(parent_dir_fd_p)
        {}

        ~input_file() noexcept
        {
            bp_sys::close(parent_dir_fd);
        }
    };

    std::stack<input_file> input_stack;

public:
    file_input_stack() = default;
    file_input_stack(const file_input_stack &) = delete;

    // Push a new input file
    // TODO limit depth and/or prevent recursion
    void push(std::string file_name, dio::istream &&file, int parent_dir_fd)
    {
        try {
            input_stack.emplace(std::move(file), std::move(file_name), 0, parent_dir_fd);
        }
        catch (...) {
            // we accept ownership of 'parent_dir_fd' at call:
            bp_sys::close(parent_dir_fd);
            throw;
        }
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

    // Get the directory to resolve file references against (from the current top-of-stack).
    int current_resolve_dir() const
    {
        return input_stack.top().parent_dir_fd;
    }
};


#endif /* DINIT_FILE_INPUT_STACK_H_INCLUDED */
