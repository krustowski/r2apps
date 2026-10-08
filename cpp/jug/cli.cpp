//
//  cli.cpp --- jug on the console: `fg jug update`, `fg jug upgrade`, ...
//

#include "app.h"
#include "fetch.h"
#include "jug.h"

#include <r2/fs.hpp>
#include <r2/io.hpp>
#include <r2/libc.hpp>
#include <r2/process.hpp>
#include <r2/time.hpp>

namespace jug {

namespace {

//  Local ports for the console's connections; the window takes 46100 on.
const uint16_t CLI_PORTS = 46000;

void usage()
{
    r2::print("jug: programs for r2, fresh from a server\n"
              "\n"
              "  jug update              fetch the list, say what is newer there\n"
              "  jug list                the list as last fetched, against what is here\n"
              "  jug upgrade [name...]   download what is newer (all of it, or these)\n"
              "  jug install name...     download programs, also ones not shipped here\n"
              "  jug remove name...      delete downloads: the shipped copy runs again\n"
              "  jug restart name...     stop the running copies and start them again\n"
              "  jug sum file...         the SHA-256 of files\n"
              "\n"
              "  --repo URL     the server (else jug.cfg's; https://cdn.vxn.dev/jug)\n"
              "  --config FILE  read another jug.cfg\n"
              "  -k             do not check the server's TLS certificate\n"
              "\n"
              "Downloads go to /mnt/tmp/jug, which fg and bg search first.\n");
}

void pad(r2::string_view s, size_t width)
{
    r2::print(s);
    for (size_t n = s.size(); n < width; n++)
        r2::out.put(' ');
}

//  "tnt", "TNT", "tnt.elf" -> "tnt"
bool nameOf(r2::string_view arg, char (&name)[NAME_CAP])
{
    if (program_name(arg, name))
        return true;
    if (!valid_name(arg))
        return false;
    for (size_t i = 0; i < arg.size(); i++)
        name[i] = arg[i] >= 'A' && arg[i] <= 'Z' ? (char)(arg[i] + 32) : arg[i];
    name[arg.size()] = 0;
    return true;
}

struct Cli
{
    Model model;
    Fetch fetch;
    bool insecure = false;
    bool usedNetwork = false;
    r2::vector<r2::string_view> names;

    //  Fetches `url`; a dot for every 64 KiB when `dots`.
    bool download(const char *url, bool dots)
    {
        usedNetwork = true;
        if (!fetch.start(url, insecure || model.config.insecure))
        {
            r2::println("jug: ", fetch.error());
            return false;
        }
        size_t shown = 0;
        while (fetch.step())
        {
            if (dots)
                for (; fetch.received() >= shown + 64 * 1024; shown += 64 * 1024)
                {
                    r2::out.put('.');
                    r2::out.flush();
                }
            r2::sleep(1);
        }
        if (fetch.ok())
            return true;
        if (dots)
            r2::println();
        r2::println("jug: ", url, ": ", fetch.error());
        return false;
    }

    bool fetchList()
    {
        r2::println("jug: list ", model.config.list);
        if (!download(model.config.list, false))
            return false;
        web::HttpResponse &r = fetch.response();
        if (!model.takeList(r.body.data, r.body.len, r.lastModified))
        {
            r2::println("jug: that is no list of programs");
            return false;
        }
        model.hashAll();
        return true;
    }

    void table()
    {
        r2::println("List  ", model.config.list);
        r2::print("      ");
        if (model.catalog.updated[0])
            r2::print("updated ", model.catalog.updated, ", ");
        r2::println(model.catalog.packages.size(), " programs");
        pad("name", 10);
        pad("size", 10);
        pad("sha256", 18);
        pad("here", 6);
        pad("state", 9);
        r2::println("running");
        for (const Row &r : model.rows)
        {
            const Package *p = model.package(r);
            const Local *l = model.local(r);
            char size[16] = {}, hex[17] = "-";
            if (p && p->size >= 0)
                scatSize(size, (uint64_t)p->size, sizeof(size));
            else if (l)
                scatSize(size, l->size, sizeof(size));
            if (p)
                p->sum.hex(hex, 16);
            pad(r.name, 10);
            pad(size, 10);
            pad(hex, 18);
            pad(l ? origin_name(l->origin) : "-", 6);
            pad(r.unreadable ? "unread" : state_name(r.state), 9);
            for (int i = 0; i < r.npids; i++)
                r2::print(i ? " " : "", r.pids[i]);
            r2::println();
        }
    }

    void summary()
    {
        int outdated = model.count(State::Outdated);
        int fresh = model.count(State::Available);
        r2::print("jug: ", model.catalog.packages.size(), " programs");
        if (model.catalog.updated[0])
            r2::print(", updated ", model.catalog.updated);
        r2::println();
        if (model.count(State::Unknown))
            r2::println("jug: some local programs could not be read; their update status is unknown");
        if (!outdated)
        {
            if (!model.count(State::Unknown))
                r2::println("jug: everything here is current", fresh ? " (`jug list` for the rest)" : "");
            return;
        }
        r2::print("jug: newer on the server:");
        for (const Row &r : model.rows)
            if (r.state == State::Outdated)
                r2::print(" ", r.name);
        r2::println("\njug: `jug upgrade` downloads them");
    }

    //  Downloads one program and puts it in place.
    bool get(const Row &row)
    {
        const Package *p = model.package(row);
        char url[URL_CAP];
        model.config.urlOf(p->path, url, sizeof(url));
        r2::print("get ", p->path, " ");
        r2::out.flush();
        if (!download(url, true))
            return false;
        web::HttpResponse &r = fetch.response();
        if (const char *why = install(*p, r.body.data, r.body.len, model.registry))
        {
            r2::println("\njug: ", row.name, ": ", why);
            return false;
        }
        char size[16] = {};
        scatSize(size, r.body.len, sizeof(size));
        r2::println(" ", size, ", sha256 ok");
        return true;
    }

    int getAll(bool upgradeOnly)
    {
        if (!fetchList())
            return 1;
        r2::vector<r2::string> wanted;
        if (names.empty())
        {
            for (const Row &r : model.rows)
                if (r.state == State::Outdated)
                    (void)wanted.push_back(r2::string(r.name));
            if (wanted.empty())
            {
                bool unknown = model.count(State::Unknown) != 0;
                r2::println(unknown ? "jug: cannot check all local programs" : "jug: everything here is current");
                return unknown ? 1 : 0;
            }
        }
        int failed = 0;
        for (r2::string_view arg : names)
        {
            char name[NAME_CAP];
            const Row *r = nameOf(arg, name) ? model.row(name) : nullptr;
            if (!r || !model.package(*r))
            {
                r2::println("jug: ", arg, ": not on the server");
                failed++;
            }
            else if (r->state == State::Current)
                r2::println("jug: ", name, " is current");
            else if (upgradeOnly && r->state == State::Available)
            {
                r2::println("jug: ", name, " is not here to upgrade: `jug install ", name, "`");
                failed++;
            }
            else
                (void)wanted.push_back(r2::string(name));
        }

        for (const r2::string &name : wanted)
        {
            Row *r = model.row(name.view());
            if (r && !get(*r))
                failed++;
        }
        model.rescan();
        for (const r2::string &name : wanted)
        {
            Row *r = model.row(name.view());
            if (r && r->npids && r->local >= 0 && model.locals[r->local].origin == Origin::Jug)
                r2::println("jug: ", name, " is running the older build: `jug restart ", name, "`");
        }
        return failed ? 1 : 0;
    }

    int remove()
    {
        int failed = 0;
        for (r2::string_view arg : names)
        {
            char name[NAME_CAP];
            const char *why = nameOf(arg, name) ? uninstall(name, model.registry) : "not a program name";
            if (why)
            {
                r2::println("jug: ", arg, ": ", why);
                failed++;
            }
            else
                r2::println("jug: removed ", name, "; the shipped copy runs from now on");
        }
        return failed ? 1 : 0;
    }

    int restartAll()
    {
        int failed = 0;
        for (r2::string_view arg : names)
        {
            char name[NAME_CAP], msg[128];
            if (!nameOf(arg, name))
            {
                r2::println("jug: ", arg, ": not a program name");
                failed++;
                continue;
            }
            if (restart(name, false, msg, sizeof(msg)) < 0)
                failed++;
            r2::println("jug: ", msg);
        }
        return failed ? 1 : 0;
    }

    int sums()
    {
        int failed = 0;
        for (r2::string_view arg : names)
        {
            char path[PATH_CAP];
            scopy(path, arg, sizeof(path));
            r2::optional<uint32_t> size = r2::fs::size_of(arg);
            Digest d;
            if (!size || !hash_file(path, *size, d))
            {
                r2::println("jug: ", arg, ": cannot be read (give the whole path)");
                failed++;
                continue;
            }
            char hex[65];
            d.hex(hex);
            r2::println(hex, "  ", arg);
        }
        return failed ? 1 : 0;
    }
};

} // namespace

int cli()
{
    use_ports(CLI_PORTS);
    Cli c;
    const char *configPath = nullptr;
    const char *repo = nullptr;
    char configBuf[PATH_CAP], repoBuf[URL_CAP];
    r2::string_view command;

    for (int i = 1; i < r2::arg_count(); i++)
    {
        r2::string_view a = r2::arg(i);
        if (a == r2::string_view("--repo") || a == r2::string_view("--config"))
        {
            bool isRepo = a == r2::string_view("--repo");
            if (i + 1 == r2::arg_count() || r2::arg(i + 1).empty() ||
                r2::arg(i + 1).size() >= (isRepo ? sizeof(repoBuf) : sizeof(configBuf)))
            {
                r2::println("jug: ", a, " needs a value that fits");
                return 1;
            }
            scopy(isRepo ? repoBuf : configBuf, r2::arg(++i), isRepo ? sizeof(repoBuf) : sizeof(configBuf));
            (isRepo ? repo : configPath) = isRepo ? repoBuf : configBuf;
        }
        else if (a == r2::string_view("-k") || a == r2::string_view("--insecure"))
            c.insecure = true;
        else if (a.starts_with("-") && a != r2::string_view("-h"))
        {
            r2::println("jug: unknown option ", a);
            return 1;
        }
        else if (command.empty())
            command = a;
        else
            (void)c.names.push_back(a);
    }

    if (command.empty() || command == r2::string_view("help") || command == r2::string_view("-h"))
    {
        usage();
        return command.empty() ? 1 : 0;
    }
    if (command == r2::string_view("sum"))
        return c.sums();

    if (!c.model.open(configPath))
    {
        r2::println("jug: cannot read ", configPath);
        return 1;
    }
    if (repo)
    {
        c.model.config.setRepo(repo);
        c.model.catalog = Catalog();
        c.model.haveList = load_list(c.model.catalog, c.model.config.list);
        c.model.rescan();
    }
    if (!mounted("/mnt/tmp"))
        r2::println("jug: there is no RAM disk at /mnt/tmp: nothing can be downloaded");

    int rc = 0;
    bool needNames = command == r2::string_view("install") || command == r2::string_view("remove") ||
                     command == r2::string_view("restart");
    if (needNames && c.names.empty())
    {
        r2::println("jug: ", command, " which programs?");
        rc = 1;
    }
    else if (command == r2::string_view("update"))
        rc = c.fetchList() ? (c.summary(), 0) : 1;
    else if (command == r2::string_view("list"))
    {
        if (!c.model.haveList)
            r2::println("jug: no list fetched since the system started: `jug update` first");
        c.model.hashAll();
        c.table();
    }
    else if (command == r2::string_view("upgrade"))
        rc = c.getAll(true);
    else if (command == r2::string_view("install"))
        rc = c.getAll(false);
    else if (command == r2::string_view("remove"))
        rc = c.remove();
    else if (command == r2::string_view("restart"))
        rc = c.restartAll();
    else
    {
        r2::println("jug: no command ", command, " (`jug help`)");
        rc = 1;
    }

    if (c.usedNetwork)
        linger(300);
    return rc;
}

} // namespace jug
