/* vim: set expandtab ts=4 sw=4: */
/*
 * You may redistribute this program and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "crypto/Key.h"
#include "dht/dhtcore/ReplySerializer.h"
#include "subnode/SupernodeHunter.h"
#include "subnode/AddrSet.h"
#include "util/Identity.h"
#include "util/platform/Sockaddr.h"
#include "util/events/Timeout.h"
#include "util/AddrTools.h"

#define CYCLE_MS 3000

struct AllocSockaddr
{
    struct Sockaddr* sa;
    struct Allocator* alloc;
    Identity
};
#define ArrayList_TYPE struct AllocSockaddr
#define ArrayList_NAME OfSnodeAddrs
#include "util/ArrayList.h"

struct SupernodeHunter_pvt
{
    struct SupernodeHunter pub;

    /** Nodes which are authorized to be our supernode. */
    struct ArrayList_OfSnodeAddrs* snodeAddrs;

    /** Our peers, DO NOT TOUCH, changed from in SubnodePathfinder. */
    struct AddrSet* peers;

    /**
     * Nodes which we have discovered.
     * When this reaches MAX, it will be flushed in onReply().
     * Flushing ensures that downed nodes will not stick around forever.
     */
    #define SupernodeHunter_pvt_nodes_MAX 64
    struct AddrSet* nodes;

    #define SupernodeHunter_pvt_snodeCandidates_MAX 8
    struct AddrSet* snodeCandidates;

    /**
     * Index in [ peers + nodes ] of node to try next.
     * (lowest bit is whether or not to send getPeers req, all higher bits are the index)
     * see pingCycle().
     */
    uint32_t nodeListIndex;

    /** Index in snodeAddrs of supernode to try next. */
    uint32_t snodeAddrIdx;

    struct Allocator* alloc;

    struct Log* log;

    struct MsgCore* msgCore;

    struct Address* myAddress;
    String* selfKeyStr;

    Identity
};

struct Query
{
    struct SupernodeHunter_pvt* snp;

    // If this is a findNode request, this is the search target, if it's a getPeers it's null.
    struct Sockaddr* searchTar;

    bool isGetRoute;

    Identity
};

static int snodeIndexOf(struct SupernodeHunter_pvt* snp, struct Sockaddr* udpAddr)
{
    for (int i = 0; i < snp->snodeAddrs->length; i++) {
        struct AllocSockaddr* as = ArrayList_OfSnodeAddrs_get(snp->snodeAddrs, i);
        if (!Sockaddr_compare(as->sa, udpAddr)) { return i; }
    }
    return -1;
}

int SupernodeHunter_addSnode(struct SupernodeHunter* snh, struct Sockaddr* udpAddr)
{
    struct SupernodeHunter_pvt* snp = Identity_check((struct SupernodeHunter_pvt*) snh);
    if (Sockaddr_getFamily(udpAddr) != Sockaddr_AF_INET6) {
        return SupernodeHunter_addSnode_INVALID_FAMILY;
    }
    if (snodeIndexOf(snp, udpAddr) != -1) { return SupernodeHunter_addSnode_EXISTS; }
    struct Allocator* alloc = Allocator_child(snp->alloc);
    struct AllocSockaddr* as = Allocator_calloc(alloc, sizeof(struct AllocSockaddr), 1);
    as->sa = Sockaddr_clone(udpAddr, alloc);
    as->alloc = alloc;
    Identity_set(as);
    ArrayList_OfSnodeAddrs_add(snp->snodeAddrs, as);
    return 0;
}

int SupernodeHunter_listSnodes(struct SupernodeHunter* snh,
                               struct Sockaddr*** outP,
                               struct Allocator* alloc)
{
    struct SupernodeHunter_pvt* snp = Identity_check((struct SupernodeHunter_pvt*) snh);
    struct Sockaddr** out = Allocator_calloc(alloc, sizeof(char*), snp->snodeAddrs->length);
    for (int i = 0; i < snp->snodeAddrs->length; i++) {
        struct AllocSockaddr* as = ArrayList_OfSnodeAddrs_get(snp->snodeAddrs, i);
        out[i] = as->sa;
    }
    *outP = out;
    return snp->snodeAddrs->length;
}

int SupernodeHunter_removeSnode(struct SupernodeHunter* snh, struct Sockaddr* toRemove)
{
    struct SupernodeHunter_pvt* snp = Identity_check((struct SupernodeHunter_pvt*) snh);
    int idx = snodeIndexOf(snp, toRemove);
    if (idx == -1) { return SupernodeHunter_removeSnode_NONEXISTANT; }
    struct AllocSockaddr* as = ArrayList_OfSnodeAddrs_get(snp->snodeAddrs, idx);
    ArrayList_OfSnodeAddrs_remove(snp->snodeAddrs, idx);
    Allocator_free(as->alloc);
    return 0;
}

static void onReply(Dict* msg, struct Address* src, struct MsgCore_Promise* prom)
{
    struct Query* q = Identity_check((struct Query*) prom->userData);
    struct SupernodeHunter_pvt* snp = Identity_check(q->snp);
    // TODO
    if (!src) {
        String* addrStr = Address_toString(prom->target, prom->alloc);
        Log_debug(snp->log, "timeout sending to %s", addrStr->bytes);
        return;
    }
    String* addrStr = Address_toString(src, prom->alloc);
    Log_debug(snp->log, "Reply from %s", addrStr->bytes);

    if (q->isGetRoute) {
        Log_debug(snp->log, "getRoute reply [%s]", addrStr->bytes);
        String* error = Dict_getStringC(msg, "error");
        if (error) {
            Log_debug(snp->log, "getRoute reply error [%s]", error->bytes);
            return;
        }
        String* labelS = Dict_getStringC(msg, "label");
        if (!labelS) {
            Log_debug(snp->log, "getRoute reply missing label");
            return;
        }
        uint64_t label = 0;
        if (labelS->len != 20 || AddrTools_parsePath(&label, labelS->bytes)) {
            Log_debug(snp->log, "getRoute reply malformed label [%s]", labelS->bytes);
            return;
        }
        if (src->path == label && Address_isSame(src, prom->target)) {
            Log_debug(snp->log, "Supernode location confirmed");
            AddrSet_add(snp->pub.snodes, src);
        } else {
            Log_debug(snp->log, "Confirming supernode location");
            src->path = label;
            AddrSet_add(snp->snodeCandidates, src);
        }
    }

    struct Address_List* results = ReplySerializer_parse(src, msg, snp->log, true, prom->alloc);
    if (!results) {
        Log_debug(snp->log, "reply without nodes");
        return;
    }
    for (int i = 0; i < results->length; i++) {
        if (!q->searchTar) {
            // This is a getPeers
            Log_debug(snp->log, "getPeers reply [%s]",
                Address_toString(&results->elems[i], prom->alloc)->bytes);
            if (Address_isSameIp(&results->elems[i], snp->myAddress)) { continue; }
            if (snp->nodes->length >= SupernodeHunter_pvt_nodes_MAX) { AddrSet_flush(snp->nodes); }
            AddrSet_add(snp->nodes, &results->elems[i]);
        } else {
            uint8_t* addrBytes;
            Assert_true(Sockaddr_getAddress(q->searchTar, &addrBytes) == 16);
            if (!Bits_memcmp(&results->elems[i].ip6.bytes, addrBytes, 16)) {
                Log_debug(snp->log, "\n\nFound a supernode w000t [%s]\n\n",
                    Address_toString(&results->elems[i], prom->alloc)->bytes);
                if (snp->snodeCandidates->length >= SupernodeHunter_pvt_snodeCandidates_MAX) {
                    AddrSet_flush(snp->snodeCandidates);
                }
                AddrSet_add(snp->snodeCandidates, &results->elems[i]);
            } else {
                //Log_debug(snp->log, "findNode reply [%s] to discard",
                //    Address_toString(&results->elems[i], prom->alloc)->bytes);
            }
        }
    }
}

static void pingCycle(void* vsn)
{
    struct SupernodeHunter_pvt* snp = Identity_check((struct SupernodeHunter_pvt*) vsn);
    if (snp->pub.snodes->length > 1) { return; }
    if (!snp->snodeAddrs->length) { return; }

    // We're not handling replies...
    struct MsgCore_Promise* qp = MsgCore_createQuery(snp->msgCore, 0, snp->alloc);
    struct Query* q = Allocator_calloc(qp->alloc, sizeof(struct Query), 1);
    Identity_set(q);
    q->snp = snp;

    Dict* msg = qp->msg = Dict_new(qp->alloc);
    qp->cb = onReply;
    qp->userData = q;

    if (snp->snodeCandidates->length) {
        qp->target = AddrSet_get(snp->snodeCandidates, snp->snodeCandidates->length - 1);
        Log_debug(snp->log, "Sending findPath to snode %s",
            Address_toString(qp->target, qp->alloc)->bytes);
        Dict_putStringCC(msg, "q", "gr", qp->alloc);
        Dict_putStringC(msg, "src", snp->selfKeyStr, qp->alloc);
        Dict_putStringC(msg, "tar", Key_stringify(qp->target->key, qp->alloc), qp->alloc);
        q->isGetRoute = true;
        return;
    }

    bool isGetPeers = snp->nodeListIndex & 1;
    int idx = snp->nodeListIndex++ >> 1;
    for (;;) {
        if (idx < snp->peers->length) {
            qp->target = AddrSet_get(snp->peers, idx);
            break;
        }
        idx -= snp->peers->length;
        if (idx < snp->nodes->length) {
            qp->target = AddrSet_get(snp->nodes, idx);
            break;
        }
        snp->snodeAddrIdx++;
        idx -= snp->nodes->length;
    }
    struct AllocSockaddr* desiredSnode =
        ArrayList_OfSnodeAddrs_get(snp->snodeAddrs, snp->snodeAddrIdx % snp->snodeAddrs->length);

    if (isGetPeers) {
        Log_debug(snp->log, "Sending getPeers to %s",
            Address_toString(qp->target, qp->alloc)->bytes);
        Dict_putStringCC(msg, "q", "gp", qp->alloc);
        Dict_putStringC(msg, "tar", String_newBinary("\0\0\0\0\0\0\0\1", 8, qp->alloc), qp->alloc);
    } else {
        q->searchTar = Sockaddr_clone(desiredSnode->sa, qp->alloc);
        Log_debug(snp->log, "Sending findNode to %s",
            Address_toString(qp->target, qp->alloc)->bytes);
        Dict_putStringCC(msg, "q", "fn", qp->alloc);
        uint8_t* snodeAddr;
        Assert_true(Sockaddr_getAddress(desiredSnode->sa, &snodeAddr) == 16);
        Dict_putStringC(msg, "tar", String_newBinary(snodeAddr, 16, qp->alloc), qp->alloc);
    }
}

struct SupernodeHunter* SupernodeHunter_new(struct Allocator* allocator,
                                            struct Log* log,
                                            struct EventBase* base,
                                            struct AddrSet* peers,
                                            struct MsgCore* msgCore,
                                            struct Address* myAddress)
{
    struct Allocator* alloc = Allocator_child(allocator);
    struct SupernodeHunter_pvt* out =
        Allocator_calloc(alloc, sizeof(struct SupernodeHunter_pvt), 1);
    out->snodeAddrs = ArrayList_OfSnodeAddrs_new(alloc);
    out->peers = peers;
    out->nodes = AddrSet_new(alloc);
    out->pub.snodes = AddrSet_new(alloc);
    out->snodeCandidates = AddrSet_new(alloc);
    out->log = log;
    out->alloc = alloc;
    out->msgCore = msgCore;
    out->myAddress = myAddress;
    out->selfKeyStr = Key_stringify(myAddress->key, alloc);
    Identity_set(out);
    Timeout_setInterval(pingCycle, out, CYCLE_MS, base, alloc);
    return &out->pub;
}