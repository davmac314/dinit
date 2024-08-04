#ifndef DINIT_FILE_INPUT_STACK_H_INCLUDED
#define DINIT_FILE_INPUT_STACK_H_INCLUDED

// Mocks for tests.

#include <fcntl.h>

#include <stack>
#include <string>
#include <sstream>

// A (mock) stack of input sources (open files) for a service description
class file_input_stack
{
    struct input_file {
        std::istringstream stream;
        std::string file_name;
        int line_num;
        std::string contents;
    };

    std::stack<input_file> input_stack;

    // read entire file contents as a string
    static std::string read_file_contents(const std::string &file_name)
    {
        std::string contents;

        int fd = open(file_name.c_str(), O_RDONLY);
        if (fd == -1) {
            throw std::system_error(errno, std::generic_category(), "read_file_contents: open");
        }

        char buf[1024];
        int r = read(fd, buf, 1024);
        while (r > 0) {
            contents.append(buf, r);
            r = read(fd, buf, 1024);
        }

        if (r == -1) {
            throw std::system_error(errno, std::generic_category(), "read_file_contents: read");
        }

        return contents;
    }

public:
    file_input_stack() = default;
    file_input_stack(const file_input_stack &) = delete;

    void add_source(std::string src, const std::string &file_name)
    {
        input_stack.emplace();
        input_file &top = input_stack.top();
        top.contents = std::move(src);
        top.stream = std::istringstream(top.contents);
        top.file_name = file_name;
        top.line_num = 0;
    }

    // loadtests currently use a real services directory
    void push(std::string file_name, std::ifstream &&file)
    {
        std::string contents = read_file_contents(file_name);
        add_source(contents, file_name);
    }

    bool getline(std::string &rline)
    {
        while (!input_stack.empty()) {
            input_file &top = input_stack.top();
            top.line_num++;

            if (std::getline(top.stream, rline)) {
                return true;
            }

            input_stack.pop();
        }
        return false;
    }

    bool getline_same_file(std::string &rline)
    {
        if (!input_stack.empty()) {
            input_file &top = input_stack.top();

            if (std::getline(top.stream, rline)) {
                top.line_num++;
                return true;
            }
        }
        return false;
    }

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
