#include <dinit-client.h>
#include <dinit-env.h>

// Get the environment from remote dinit instance
void get_remote_env(int csfd, cpbuffer_t &rbuffer, environment &menv)
{
    char buf[2] = { (char)cp_cmd::GETALLENV, 0 };
    write_all_x(csfd, buf, 2);

    wait_for_reply(rbuffer, csfd);

    if (rbuffer[0] != (char)cp_rply::ALLENV) {
        throw dinit_protocol_error();
    }

    // 1-byte packet header, then size_t data size
    constexpr size_t allenv_hdr_size = 1 + sizeof(size_t);
    rbuffer.fill_to(csfd, allenv_hdr_size);

    size_t data_size;
    rbuffer.extract(&data_size, 1, sizeof(data_size));
    rbuffer.consume(allenv_hdr_size);

    if (data_size == 0) return;

    if (rbuffer.get_length() == 0) {
        fill_some(rbuffer, csfd);
    }

    std::string env_var;

    while (data_size > 0) {
        // look for a nul terminator
        get_var:
        unsigned contig_len = rbuffer.get_contiguous_length(rbuffer.get_ptr(0));
        unsigned check_len = std::min((size_t) contig_len, data_size);
        for (unsigned i = 0; i < check_len; ++i) {
            if (rbuffer[i] == '\0') {
                // append the last portion
                env_var.append(rbuffer.get_ptr(0), rbuffer.get_ptr(0) + i);
                rbuffer.consume(i + 1);
                data_size -= (i + 1);

                menv.set_var(std::move(env_var));
                env_var.clear();

                if (data_size == 0) {
                    // that's the last one
                    return;
                }

                goto get_var;
            }
        }

        // copy what we have so far to the string, and fill some more
        env_var.append(rbuffer.get_ptr(0), rbuffer.get_ptr(0) + check_len);
        rbuffer.consume(check_len);
        data_size -= check_len;

        if (data_size == 0) {
            // This shouldn't happen, we didn't find the nul terminator at the end
            throw dinit_protocol_error();
        }

        if (rbuffer.get_length() == 0) {
            fill_some(rbuffer, csfd);
        }
    }
}
