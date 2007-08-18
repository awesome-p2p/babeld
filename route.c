/*
Copyright (c) 2007 by Juliusz Chroboczek

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <assert.h>

#include "babel.h"
#include "util.h"
#include "kernel.h"
#include "source.h"
#include "neighbour.h"
#include "route.h"
#include "xroute.h"
#include "message.h"

struct route routes[MAXROUTES];
int numroutes = 0;
int kernel_metric = 0;
int route_timeout_delay = 160;
int route_gc_delay = 180;

struct route *
find_route(const unsigned char *prefix, unsigned char plen,
           struct neighbour *nexthop)
{
    int i;
    for(i = 0; i < numroutes; i++) {
        if(routes[i].nexthop == nexthop &&
           source_match(routes[i].src, prefix, plen))
            return &routes[i];
    }
    return NULL;
}

struct route *
find_installed_route(const unsigned char *prefix, unsigned char plen)
{
    int i;
    for(i = 0; i < numroutes; i++) {
        if(routes[i].installed && source_match(routes[i].src, prefix, plen))
            return &routes[i];
    }
    return NULL;
}

void
flush_route(struct route *route)
{
    int n;
    struct source *src;
    int oldmetric, lost = 0;

    n = route - routes;
    assert(n >= 0 && n < numroutes);

    oldmetric = route->metric;

    if(route->installed) {
        uninstall_route(route);
        lost = 1;
    }

    src = route->src;

    if(n != numroutes - 1)
        memcpy(routes + n, routes + numroutes - 1, sizeof(struct route));
    numroutes--;
    VALGRIND_MAKE_MEM_UNDEFINED(routes + numroutes, sizeof(struct route));

    if(lost)
        route_lost(src, oldmetric);
}

void
flush_neighbour_routes(struct neighbour *neigh)
{
    int i;

    i = 0;
    while(i < numroutes) {
        if(routes[i].nexthop == neigh) {
            flush_route(routes + i);
            continue;
        }
        i++;
    }
}

unsigned int
metric_to_kernel(int metric)
{
    assert(metric >= 0);

    if(metric >= INFINITY)
        return KERNEL_INFINITY;
    else
        return MIN((metric + 255) / 256 + kernel_metric, KERNEL_INFINITY);
}

void
install_route(struct route *route)
{
    int rc;

    if(route->installed)
        return;

    rc = kernel_route(ROUTE_ADD, route->src->prefix, route->src->plen,
                      route->nexthop->address,
                      route->nexthop->network->ifindex,
                      metric_to_kernel(route->metric), NULL, 0, 0);
    if(rc < 0) {
        perror("kernel_route(ADD)");
        if(errno != EEXIST)
            return;
    }
    route->installed = 1;
}

void
uninstall_route(struct route *route)
{
    int rc;

    if(!route->installed)
        return;

    rc = kernel_route(ROUTE_FLUSH, route->src->prefix, route->src->plen,
                      route->nexthop->address,
                      route->nexthop->network->ifindex,
                      metric_to_kernel(route->metric), NULL, 0, 0);
    if(rc < 0)
        perror("kernel_route(FLUSH)");

    route->installed = 0;
}

/* This is equivalent to uninstall_route followed with install_route,
   but without the race condition.  The destination of both routes
   must be the same. */

void
change_route(struct route *old, struct route *new)
{
    int rc;

    if(!old) {
        install_route(new);
        return;
    }

    if(!old->installed)
        return;

    rc = kernel_route(ROUTE_MODIFY, old->src->prefix, old->src->plen,
                      old->nexthop->address, old->nexthop->network->ifindex,
                      metric_to_kernel(old->metric),
                      new->nexthop->address, new->nexthop->network->ifindex,
                      metric_to_kernel(new->metric));
    if(rc >= 0) {
        old->installed = 0;
        new->installed = 1;
    }
}

void
change_route_metric(struct route *route, int newmetric)
{
    int rc;

    if(route->installed) {
        rc = kernel_route(ROUTE_MODIFY,
                          route->src->prefix, route->src->plen,
                          route->nexthop->address,
                          route->nexthop->network->ifindex,
                          metric_to_kernel(route->metric),
                          route->nexthop->address,
                          route->nexthop->network->ifindex,
                          metric_to_kernel(newmetric));
        if(rc < 0) {
            perror("kernel_route(MODIFY)");
            return;
        }
    }
    route->metric = newmetric;
}

int
route_feasible(struct route *route)
{
    return update_feasible(route->src->address,
                           route->src->prefix, route->src->plen,
                           route->seqno, route->refmetric);
}

int
update_feasible(const unsigned char *a,
                const unsigned char *p, unsigned char plen,
                unsigned short seqno, unsigned short refmetric)
{
    struct source *src = find_source(a, p, plen, 0, 0);
    if(src == NULL)
        return 1;

    if(src->time < now.tv_sec - 200)
        /* Never mind what is probably stale data */
        return 1;

    if(refmetric >= INFINITY)
        /* Retractions are always feasible */
        return 1;

    return (seqno_compare(seqno, src->seqno) > 0 ||
            (src->seqno == seqno && refmetric < src->metric));
}

/* This returns a feasible route.  The only condition it must satisfy if that
   if there is a feasible route, then one will be found. */
struct route *
find_best_route(const unsigned char *prefix, unsigned char plen)
{
    struct route *route = NULL;
    int i;

    for(i = 0; i < numroutes; i++) {
        if(!source_match(routes[i].src, prefix, plen))
            continue;
        if(routes[i].time < now.tv_sec - route_timeout_delay)
            continue;
        if(!route_feasible(&routes[i]))
            continue;
        if(route && route->metric < INFINITY &&
           route->metric + 512 >= routes[i].metric) {
            if(route->origtime <= now.tv_sec - 30 &&
               routes[i].origtime >= now.tv_sec - 30)
                continue;
            if(route->metric < routes[i].metric)
                continue;
            if(route->origtime > routes[i].origtime)
                continue;
        }
        route = &routes[i];
    }
    return route;
}

void
update_route_metric(struct route *route)
{
    int oldmetric;
    int newmetric;

    oldmetric = route->metric;
    if(route->time < now.tv_sec - route_timeout_delay) {
        if(route->refmetric < INFINITY) {
            route->seqno = (route->src->seqno + 1) & 0xFFFF;
            route->refmetric = INFINITY;
        }
        newmetric = INFINITY;
    } else {
        newmetric = MIN(route->refmetric + neighbour_cost(route->nexthop),
                        INFINITY);
    }

    change_route_metric(route, newmetric);
    trigger_route_change(route, route->src, oldmetric);
}

void
update_neighbour_metric(struct neighbour *neigh)
{
    int i;

    i = 0;
    while(i < numroutes) {
        if(routes[i].nexthop == neigh)
            update_route_metric(&routes[i]);
        i++;
    }
}

/* This is called whenever we receive an update. */
struct route *
update_route(const unsigned char *a, const unsigned char *p, unsigned char plen,
             unsigned short seqno, unsigned short refmetric,
             struct neighbour *nexthop)
{
    struct route *route;
    struct source *src;
    int metric, feasible;

    if(martian_prefix(p, plen)) {
        fprintf(stderr, "Rejecting martian route to %s through %s.\n",
                format_prefix(p, plen), format_address(a));
        return NULL;
    }

    src = find_source(a, p, plen, 1, seqno);
    if(src == NULL)
        return NULL;

    feasible = update_feasible(a, p, plen, seqno, refmetric);
    route = find_route(p, plen, nexthop);
    metric = MIN((int)refmetric + neighbour_cost(nexthop), INFINITY);

    if(route) {
        struct source *oldsrc;
        unsigned short oldseqno;
        unsigned short oldmetric;
        int lost = 0;

        oldsrc = route->src;
        oldseqno = route->seqno;
        oldmetric = route->metric;

        /* If a successor switches sources, we must accept his update even
           if it makes a route unfeasible in order to break any routing loops.
           It's not clear to me (jch) what is the best approach if the
           successor sticks to the same source but increases its metric. */
        if(!feasible && route->installed) {
            debugf("Unfeasible update for installed route to %s "
                   "(%s %d %d -> %s %d %d).\n",
                   format_prefix(src->prefix, src->plen),
                   format_address(route->src->address),
                   route->seqno, route->refmetric,
                   format_address(src->address), seqno, refmetric);
            uninstall_route(route);
            lost = 1;
        }

        route->src = src;
        if(feasible && refmetric < INFINITY) {
            route->time = now.tv_sec;
            if(route->refmetric >= INFINITY)
                route->origtime = now.tv_sec;
        }
        route->seqno = seqno;
        route->refmetric = refmetric;
        change_route_metric(route, metric);

        if(feasible)
            trigger_route_change(route, oldsrc, oldmetric);
        else if(lost)
            route_lost(oldsrc, oldmetric);
    } else {
        if(!feasible)
            return NULL;
        if(refmetric >= INFINITY)
            /* Somebody's retracting a route we never saw. */
            return NULL;
        if(numroutes >= MAXROUTES) {
            fprintf(stderr, "Too many routes -- ignoring update.\n");
            return NULL;
        }
        route = &routes[numroutes];
        route->src = src;
        route->refmetric = refmetric;
        route->seqno = seqno;
        route->metric = metric;
        route->nexthop = nexthop;
        route->time = now.tv_sec;
        route->origtime = now.tv_sec;
        route->installed = 0;
        numroutes++;
        consider_route(route);
    }
    return route;
}

/* This takes a feasible route and decides whether to install it.  The only
   condition that it must satisfy is that if there is no currently installed
   route, then one will be installed. */
void
consider_route(struct route *route)
{
    struct route *installed;

    if(route->installed)
        return;

    if(!route_feasible(route))
        return;

    if(find_exported_xroute(route->src->prefix, route->src->plen))
       return;

    installed = find_installed_route(route->src->prefix, route->src->plen);

    if(installed == NULL)
        goto install;

    if(installed->metric >= route->metric + 384)
        goto install;

    /* Avoid routes that haven't been around for some time */
    if(route->origtime >= now.tv_sec - 30)
        return;

    if(installed->metric >= route->metric + 192)
        goto install;

    /* Avoid switching sources */
    if(installed->src != route->src)
        return;

    if(installed->metric >= route->metric + 96)
        goto install;

    return;

 install:
    change_route(installed, route);
    if(installed && route->installed)
        send_triggered_update(route, installed->src, installed->metric);
    else
        send_update(NULL, route->src->prefix, route->src->plen);
    return;
}

void
send_triggered_update(struct route *route, struct source *oldsrc, int oldmetric)
{
    if(!route->installed)
        return;

    /* Switching sources can cause transient routing loops, so always send
       updates in that case */
    if(route->src != oldsrc ||
       ((route->metric >= INFINITY) != (oldmetric >= INFINITY)) ||
       (route->metric >= oldmetric + 256 || oldmetric >= route->metric + 256))
        send_update(NULL, route->src->prefix, route->src->plen);

    if(oldmetric < INFINITY) {
        if(route->metric >= INFINITY || route->metric - oldmetric >= 384)
            send_request(NULL, route->src->prefix, route->src->plen);
    }
}

/* A route has just changed.  Decide whether to switch to a different route or
   send an update. */
void
trigger_route_change(struct route *route,
                     struct source *oldsrc, unsigned short oldmetric)
{
    if(route->installed) {
        if(route->metric > oldmetric) {
            struct route *better_route;
            better_route =
                find_best_route(route->src->prefix, route->src->plen);
            if(better_route && better_route->metric <= route->metric - 96)
                consider_route(better_route);
        }

        if(route->installed)
            send_triggered_update(route, oldsrc, oldmetric);

        return;
    }

    /* consider_route avoids very recent routes, so reconsider newish routes
       even when their metric didn't decrease. */
    if(route->metric < oldmetric || route->origtime >= now.tv_sec - 240)
        consider_route(route);
}

/* We just lost the installed route to a given destination. */
void
route_lost(struct source *src, int oldmetric)
{
    struct route *new_route;
    new_route = find_best_route(src->prefix, src->plen);
    if(new_route) {
        consider_route(new_route);
    } else {
        /* Complain loudly. */
        send_update(NULL, src->prefix, src->plen);
        if(oldmetric < INFINITY)
            send_request(NULL, src->prefix, src->plen);
    }
}

void
expire_routes(void)
{
    int i;

    debugf("Expiring old routes.\n");

    i = 0;
    while(i < numroutes) {
        struct route *route = &routes[i];

        if(route->time < now.tv_sec - route_gc_delay) {
            flush_route(route);
            continue;
        }

        update_route_metric(route);

        if(route->installed && route->refmetric < INFINITY) {
            if(route->time < now.tv_sec - MAX(10, route_timeout_delay - 25))
                send_unicast_request(route->nexthop,
                                     route->src->prefix, route->src->plen);
        }
        i++;
    }
}
