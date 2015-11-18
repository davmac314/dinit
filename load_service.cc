#include "service.h"
#include <string>
#include <fstream>
#include <locale>
#include <iostream>

typedef std::string string;
typedef std::string::iterator string_iterator;

// Utility function to skip white space. Returns an iterator at the
// first non-white-space position (or at end).
static string_iterator skipws(string_iterator i, string_iterator end)
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
static string read_setting_name(string_iterator & i, string_iterator end)
{
    using std::locale;
    using std::ctype;
    using std::use_facet;
    
    const ctype<char> & facet = use_facet<ctype<char> >(locale::classic());

    string rval;
    // Allow alphabetical characters, and dash (-) in setting name
    while (i != end && (*i == '-' || facet.is(ctype<char>::alpha, *i))) {
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
//    i  -  reference to string iterator through the line
//    end -   iterator at end of line
//    part_positions -  list of <int,int> to which the position of each setting value
//                      part will be added as [start,end). May be null.
static string read_setting_value(string_iterator & i, string_iterator end,
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
                    // TODO error here.
                }
                else if (c == '\\') {
                    // A backslash escapes the following character.
                    ++i;
                    if (i != end) {
                        c = *i;
                        if (c == '\n') {
                            // TODO error here.
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
                // TODO error here
                break;
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
                // TODO error here
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
            // hmm... comment? Probably, though they should have put a space
            // before it really. TODO throw an exception, and document
            // that '#' for comments must be preceded by space, and in values
            // must be quoted.
            break;
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


// Find a service record, or load it from file. If the service has
// dependencies, load those also.
//
// Might throw a ServiceLoadExc exception if a dependency cycle is found or if another
// problem occurs (I/O error, service description not found etc). Throws std::bad_alloc
// if a memory allocation failure occurs.
ServiceRecord * ServiceSet::loadServiceRecord(const char * name)
{
    using std::string;
    using std::ifstream;
    using std::ios;
    using std::ios_base;
    using std::locale;
    using std::isspace;
    
    // First try and find an existing record...
    ServiceRecord * rval = findService(string(name));
    if (rval != 0) {
        if (rval->isDummy()) {
            throw ServiceCyclicDependency(name);
        }
        return rval;
    }

    // Couldn't find one. Have to load it.    
    string service_filename = service_dir;
    if (*(service_filename.rbegin()) != '/') {
        service_filename += '/';
    }
    service_filename += name;
    
    string command;
    std::list<std::pair<unsigned,unsigned>> command_offsets;

    ServiceType service_type = ServiceType::PROCESS;
    std::list<ServiceRecord *> depends_on;
    std::list<ServiceRecord *> depends_soft;
    string logfile;
    OnstartFlags onstart_flags;
    
    string line;
    bool auto_restart = false;
    ifstream service_file;
    service_file.exceptions(ios::badbit | ios::failbit);
    
    try {
        service_file.open(service_filename.c_str(), ios::in);
    }
    catch (std::ios_base::failure &exc) {
        throw ServiceNotFound(name);
    }
    
    // Add a dummy service record now to prevent infinite recursion in case of cyclic dependency
    rval = new ServiceRecord(this, string(name));
    records.push_back(rval);
    
    // getline can set failbit if it reaches end-of-file, we don't want an exception in that case:
    service_file.exceptions(ios::badbit);
    
    while (! (service_file.rdstate() & ios::eofbit)) {
        getline(service_file, line);
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
                throw ServiceDescriptionExc(name, "Badly formed line.");
            }
            i = skipws(++i, end);
            
            if (setting == "command") {
                command = read_setting_value(i, end, &command_offsets);
            }
            else if (setting == "depends-on") {
                string dependency_name = read_setting_value(i, end);
                depends_on.push_back(loadServiceRecord(dependency_name.c_str()));
            }
            else if (setting == "waits-for") {
                string dependency_name = read_setting_value(i, end);
                depends_soft.push_back(loadServiceRecord(dependency_name.c_str()));
            }
            else if (setting == "logfile") {
                logfile = read_setting_value(i, end);
            }
            else if (setting == "restart") {
                string restart = read_setting_value(i, end);
                auto_restart = (restart == "yes" || restart == "true");
            }
            else if (setting == "type") {
                string type_str = read_setting_value(i, end);
                if (type_str == "scripted") {
                    service_type = ServiceType::SCRIPTED;
                }
                else if (type_str == "process") {
                    service_type = ServiceType::PROCESS;
                }
                else if (type_str == "internal") {
                    service_type = ServiceType::INTERNAL;
                }
                else {
                    throw ServiceDescriptionExc(name, "Service type must be \"scripted\""
                        " or \"process\" or \"internal\"");
                }
            }
            else if (setting == "onstart") {
                std::list<std::pair<unsigned,unsigned>> indices;
                string onstart_cmds = read_setting_value(i, end, &indices);
                for (auto indexpair : indices) {
                    string onstart_cmd = onstart_cmds.substr(indexpair.first, indexpair.second - indexpair.first);
                    if (onstart_cmd == "release_console") {
                        onstart_flags.release_console = true;
                    }
                    else if (onstart_cmd == "rw_ready") {
                        onstart_flags.rw_ready = true;
                    }
                    else {
                        throw new ServiceDescriptionExc(name, "Unknown onstart command: " + onstart_cmd);
                    }
                }
            }
            else {
                throw ServiceDescriptionExc(name, "Unknown setting: " + setting);
            }
        }
    }
    
    service_file.close();
    // TODO check we actually have all the settings - type, command
    
    // Now replace the dummy service record with a real record:
    for (auto iter = records.begin(); iter != records.end(); iter++) {
        if (*iter == rval) {
            // We've found the dummy record
            delete rval;
            rval = new ServiceRecord(this, string(name), service_type, std::move(command), command_offsets,
                    & depends_on, & depends_soft);
            rval->setLogfile(logfile);
            rval->setAutoRestart(auto_restart);
            rval->setOnstartFlags(onstart_flags);
            *iter = rval;
            break;
        }
    }
    
    return rval;
}
