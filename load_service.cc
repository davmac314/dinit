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
static string read_setting_name(string_iterator * const i, string_iterator end)
{
    using std::locale;
    using std::ctype;
    using std::use_facet;
    
    const ctype<char> & facet = use_facet<ctype<char> >(locale::classic());

    string rval;
    // Allow alphabetical characters, and dash (-) in setting name
    while (*i != end && (**i == '-' || facet.is(ctype<char>::alpha, **i))) {
        rval += **i;
        ++(*i);
    }
    return rval;
}

// Read a setting value
// Try to allow quoted strings:
static string read_setting_value(string_iterator * const i, string_iterator end)
{
    // TODO handle quoting, error if multiple white-space separated strings
    // occur without quoting (unless the second one is a '#' comment)
    using std::locale;
    using std::isspace;

    *i = skipws(*i, end);
    
    string rval;
    // bool quoting = false;
    
    while (*i != end) {
        char c = **i;
        if (c == '\"') {
            // quoted ...
            // TODO
        }
        if (isspace(c, locale::classic())) {
            *i = skipws(*i, end);
            if (*i == end) break;
            if (**i == '#') break; // comment
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
            rval += c;
        }
        ++(*i);
    }
    
    return rval;
}


// Find a service record, or load it from file. If the service has
// dependencies, load those also.
//
// Might throw a ServiceLoadExc exception if a dependency cycle is found or if another
// problem occurs (I/O error, service description not found etc).
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
    int service_type = SVC_PROCESS;
    std::list<ServiceRecord *> depends_on;
    std::list<ServiceRecord *> depends_soft;
    string logfile;
    
    // TODO catch I/O exceptions, wrap & re-throw?
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
    
    // Add a dummy service record now to prevent cyclic dependencies
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
            string setting = read_setting_name(&i, end);
            i = skipws(i, end);
            if (i == end || *i != '=') {
                throw ServiceDescriptionExc(name, "Badly formed line.");
            }
            i = skipws(++i, end);
            
            if (setting == "command") {
                command = read_setting_value(&i, end);
            }
            else if (setting == "depends-on") {
                string dependency_name = read_setting_value(&i, end);
                depends_on.push_back(loadServiceRecord(dependency_name.c_str()));
            }
            else if (setting == "depends-soft") {
                string dependency_name = read_setting_value(&i, end);
                depends_soft.push_back(loadServiceRecord(dependency_name.c_str()));
            }
            else if (setting == "logfile") {
                logfile = read_setting_value(&i, end);
            }
            else if (setting == "restart") {
                string restart = read_setting_value(&i, end);
                auto_restart = (restart == "yes" || restart == "true");
            }
            else if (setting == "type") {
                string type_str = read_setting_value(&i, end);
                if (type_str == "scripted") {
                    service_type = SVC_SCRIPTED;
                }
                else if (type_str == "process") {
                    service_type = SVC_PROCESS;
                }
                else {
                    throw ServiceDescriptionExc(name, "Service type must be \"scripted\""
                        " or \"process\"");
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
            rval = new ServiceRecord(this, string(name), service_type, command,
                    & depends_on, & depends_soft);
            rval->setLogfile(logfile);
            rval->setAutoRestart(auto_restart);
            *iter = rval;
            break;
        }
    }
    
    return rval;
}
