#include <iostream>
#include <list>

// Exception while loading a service
class service_load_exc
{
    public:
    std::string service_name;
    std::string exc_description;

    protected:
    service_load_exc(const std::string &serviceName, std::string &&desc) noexcept
        : service_name(serviceName), exc_description(std::move(desc))
    {
    }
};

class service_not_found : public service_load_exc
{
    public:
    service_not_found(const std::string &serviceName) noexcept
        : service_load_exc(serviceName, "Service description not found.")
    {
    }
};

class service_cyclic_dependency : public service_load_exc
{
    public:
    service_cyclic_dependency(const std::string &serviceName) noexcept
        : service_load_exc(serviceName, "Has cyclic dependency.")
    {
    }
};

class service_description_exc : public service_load_exc
{
    public:
    service_description_exc(const std::string &serviceName, std::string &&extraInfo) noexcept
        : service_load_exc(serviceName, std::move(extraInfo))
    {
    }
};

namespace dinit_load {

using string = std::string;
using string_iterator = std::string::iterator;

// exception thrown when encountering a syntax issue when reading a setting value
class setting_exception
{
    std::string info;

    public:
    setting_exception(const std::string &&exc_info) : info(std::move(exc_info))
    {
    }

    std::string &get_info()
    {
        return info;
    }
};


// Utility function to skip white space. Returns an iterator at the
// first non-white-space position (or at end).
inline string_iterator skipws(string_iterator i, string_iterator end)
{
    using std::locale;
    using std::isspace;

    while (i != end) {
      if (! isspace(*i, locale::classic())) {
        break;
      }
      ++i;
    }
    return i;
}

// Read a setting name.
inline string read_setting_name(string_iterator & i, string_iterator end)
{
    using std::locale;
    using std::ctype;
    using std::use_facet;

    const ctype<char> & facet = use_facet<ctype<char> >(locale::classic());

    string rval;
    // Allow alphabetical characters, and dash (-) in setting name
    while (i != end && (*i == '-' || *i == '.' || facet.is(ctype<char>::alpha, *i))) {
        rval += *i;
        ++i;
    }
    return rval;
}

// Read a setting value
//
// In general a setting value is a single-line string. It may contain multiple parts
// separated by white space (which is normally collapsed). A hash mark - # - denotes
// the end of the value and the beginning of a comment (it should be preceded by
// whitespace).
//
// Part of a value may be quoted using double quote marks, which prevents collapse
// of whitespace and interpretation of most special characters (the quote marks will
// not be considered part of the value). A backslash can precede a character (such
// as '#' or '"' or another backslash) to remove its special meaning. Newline
// characters are not allowed in values and cannot be quoted.
//
// This function expects the string to be in an ASCII-compatible, single byte
// encoding (the "classic" locale).
//
// Params:
//    service_name - the name of the service to which the setting applies
//    i  -  reference to string iterator through the line
//    end -   iterator at end of line
//    part_positions -  list of <int,int> to which the position of each setting value
//                      part will be added as [start,end). May be null.
inline string read_setting_value(string_iterator & i, string_iterator end,
        std::list<std::pair<unsigned,unsigned>> * part_positions = nullptr)
{
    using std::locale;
    using std::isspace;

    i = skipws(i, end);

    string rval;
    bool new_part = true;
    int part_start;

    while (i != end) {
        char c = *i;
        if (c == '\"') {
            if (new_part) {
                part_start = rval.length();
                new_part = false;
            }
            // quoted string
            ++i;
            while (i != end) {
                c = *i;
                if (c == '\"') break;
                if (c == '\n') {
                    throw setting_exception("Line end inside quoted string");
                }
                else if (c == '\\') {
                    // A backslash escapes the following character.
                    ++i;
                    if (i != end) {
                        c = *i;
                        if (c == '\n') {
                            throw setting_exception("Line end follows backslash escape character (`\\')");
                        }
                        rval += c;
                    }
                }
                else {
                    rval += c;
                }
                ++i;
            }
            if (i == end) {
                // String wasn't terminated
                throw setting_exception("Unterminated quoted string");
            }
        }
        else if (c == '\\') {
            if (new_part) {
                part_start = rval.length();
                new_part = false;
            }
            // A backslash escapes the next character
            ++i;
            if (i != end) {
                rval += *i;
            }
            else {
                throw setting_exception("Backslash escape (`\\') not followed by character");
            }
        }
        else if (isspace(c, locale::classic())) {
            if (! new_part && part_positions != nullptr) {
                part_positions->emplace_back(part_start, rval.length());
                new_part = true;
            }
            i = skipws(i, end);
            if (i == end) break;
            if (*i == '#') break; // comment
            rval += ' ';  // collapse ws to a single space
            continue;
        }
        else if (c == '#') {
            // Possibly intended a comment; we require leading whitespace to reduce occurrence of accidental
            // comments in setting values.
            throw setting_exception("hashmark (`#') comment must be separated from setting value by whitespace");
        }
        else {
            if (new_part) {
                part_start = rval.length();
                new_part = false;
            }
            rval += c;
        }
        ++i;
    }

    // Got to end:
    if (part_positions != nullptr) {
        part_positions->emplace_back(part_start, rval.length());
    }

    return rval;
}

// Process an opened service file, line by line.
//    name - the service name
//    service_file - the service file input stream
//    func - a function of the form:
//             void(string &line, string &setting, string_iterator i, string_iterator end)
//           Called with:
//               line - the complete line
//               setting - the setting name, from the beginning of the line
//               i - iterator at the beginning of the setting value
//               end - iterator marking the end of the line
//
// May throw service load exceptions or I/O exceptions if enabled on stream.
template <typename T>
void process_service_file(string name, std::istream &service_file, T func)
{
    string line;

    while (getline(service_file, line)) {
        string::iterator i = line.begin();
        string::iterator end = line.end();

        i = skipws(i, end);
        if (i != end) {
            if (*i == '#') {
                continue;  // comment line
            }
            string setting = read_setting_name(i, end);
            i = skipws(i, end);
            if (i == end || (*i != '=' && *i != ':')) {
                throw service_description_exc(name, "Badly formed line.");
            }
            i = skipws(++i, end);

            func(line, setting, i, end);
        }
    }
}

} // namespace dinit_load

using dinit_load::process_service_file;
