//
//  fetch.cpp --- see fetch.h.
//

#include "fetch.h"

#include "jug.h"
#include "net_r2.h"

#include <r2/time.hpp>

namespace jug {

Fetch::Fetch() : loader_(web::r2Net(), web::gatherEntropy, web::currentTime) {}

bool Fetch::start(const char *url, bool insecure)
{
    error_[0] = 0;
    web::Url u;
    if (!web::urlFromInput(url, u))
    {
        scopy(error_, "not a URL: ", sizeof(error_));
        scat(error_, url, sizeof(error_));
        return false;
    }
    loader_.start(u, insecure);
    return true;
}

bool Fetch::step()
{
    loader_.step();
    return loader_.busy();
}

void Fetch::cancel() { loader_.cancel(); }

bool Fetch::ok() const
{
    if (loader_.phase() != web::Loader::DONE)
        return false;
    web::HttpResponse &r = const_cast<web::Loader &>(loader_).response();
    return r.status == 200 && !r.truncated && !r.failed && !r.body.failed;
}

const char *Fetch::error() const
{
    if (error_[0])
        return error_;
    if (loader_.phase() == web::Loader::FAILED)
        return loader_.error();
    if (loader_.phase() != web::Loader::DONE)
        return "";
    web::HttpResponse &r = const_cast<web::Loader &>(loader_).response();
    error_[0] = 0;
    if (r.status != 200)
    {
        scopy(error_, "the server answered ", sizeof(error_));
        scatU(error_, (uint64_t)r.status, sizeof(error_));
        if (r.reason[0])
        {
            scat(error_, " ", sizeof(error_));
            scat(error_, r.reason, sizeof(error_));
        }
    }
    else if (r.truncated)
        scopy(error_, "larger than the 4 MiB a download may be", sizeof(error_));
    else if (r.body.failed)
        scopy(error_, "no memory left for it", sizeof(error_));
    else if (r.failed)
        scopy(error_, r.error[0] ? r.error : "the HTTP response was incomplete", sizeof(error_));
    return error_;
}

void use_ports(uint16_t base) { web::r2NetSetPortBase(base); }

void linger(uint32_t ms)
{
    uint64_t until = r2::ticks() + ms;
    while (r2::ticks() < until)
    {
        web::r2Net().poll();
        r2::sleep(1);
    }
}

} // namespace jug
