#include "mega/sccloudraid/raidproxy.h"
#include "mega/sccloudraid/mega.h"
#include <algorithm>
#include <map>
#include <sstream>

std::atomic<mega::SCCR::raidTime> currtime;

using namespace mega::SCCR;

#define MAX_DELAY_IN_SECONDS 30

std::atomic<uint64_t> PartFetcher::globalBytesReceived;

PartFetcher::PartFetcher()
{
    rr = NULL;
    connected = false;
    finished = false;
    skip_setposrem = false;

    partStartPos = 0;
    pos = 0;
    rem = 0;
    remfeed = 0;

    errors = 0;
    consecutive_errors = 0;
    lastdata = currtime;
    lastconnect = 0;
    reqBytesReceived = 0;
    timeInflight = 0;
    postCompleted = false;

    delayuntil = 0;
}

PartFetcher::~PartFetcher()
{
    static m_off_t globalReqBytesReceived = 0;
    static m_off_t globalTimeInflight = 0;
    static m_off_t globalAccSpeed = 0;
    std::cout << "[PartFetcher::~PartFetcher] part = " << std::to_string(part) << std::endl;
    if (rr)
    {
        globalReqBytesReceived += reqBytesReceived;
        globalTimeInflight += timeInflight;
        globalAccSpeed += getSocketSpeed();
        std::cout << "[PartFetcher::~PartFetcher] part = " << std::to_string(part) << " [httpReq = " << rr->httpReqs[part] << ", speed = " << ((getSocketSpeed() * 1000) / 1024) << " KB/s" << std::endl;
        closesocket();

        while (!readahead.empty())
        {
            free(readahead.begin()->second.first);
            readahead.erase(readahead.begin());
        }
    }
    if (part == 0) std::cout << "[PartFetcher::~PartFetcher] GLOBAL AVERAGE SPEED = " << (globalTimeInflight ? (((globalReqBytesReceived / globalTimeInflight) * 1000) / 1024) : 0) << " KB/s [totalBytesReceived = " << globalReqBytesReceived << ", totalTimeInflight = " << globalTimeInflight << "]" << std::endl;
}

bool PartFetcher::setsource(const std::string& partUrl, RaidReq* crr, int cpart)
{
    url = partUrl;
    part = cpart;
    rr = crr;
    partStartPos = rr->reqStartPos / (RAIDPARTS-1);
    std::cout << "[PartFetcher::setsource] partStartPos = " << partStartPos << ", MOD RAIDSECTOR(16) = " << (partStartPos % RAIDSECTOR) << " MOD RAIDLINE(80) = " << (partStartPos % RAIDLINE) << " [rr->reqStartPos = " << rr->reqStartPos << "]" << std::endl;
    assert(partStartPos % RAIDSECTOR == 0);

    sourcesize = RaidReq::raidPartSize(part, rr->filesize);
    return true;
}

size_t RaidReq::raidPartSize(int part, size_t fullfilesize)
{
    // compute the size of this raid part based on the original file size len
    int r = fullfilesize % RAIDLINE;   // residual part

    // parts 0 (parity) & 1 (largest data) are the same size
    int t = r-(part-!!part)*RAIDSECTOR;

    // (excess length will be found in the following sectors,
    // negative length in the preceding sectors)
    if (t < 0) t = 0;
    else if (t > RAIDSECTOR) t = RAIDSECTOR;

    return (fullfilesize-r)/(RAIDPARTS-1)+t;
}

#define OWNREADAHEAD 64
#define ISOWNREADAHEAD(X) ((X) & OWNREADAHEAD)
#define VALIDPARTS(X) ((X) & (OWNREADAHEAD-1))

#define SECTORFLOOR(X) ((X) & -RAIDSECTOR)

// sets the next read position (pos) and the remaining read length (rem/remfeed),
// taking into account all readahead data and ongoing reads on other connected
// fetchers.
void PartFetcher::setposrem()
{
    std::cout << "Setposrem [part = " << std::to_string(part) << "]" << std::endl;
    // we want to continue reading at the 2nd lowest position:
    // take the two lowest positions and use the higher one.
    static thread_local map<m_off_t, char> chunks;
    m_off_t basepos = rr->dataline*RAIDSECTOR;
    m_off_t curpos = basepos+rr->partpos[(int)part];
    std::cout << "Setposrem [part = " << std::to_string(part) << "] basepos="<<basepos<<", curpos=" << curpos << ", rr->partpos["<<(int)part<<"]=" << rr->partpos[(int)part] << "]" << std::endl;

    // determine the next suitable read range to ensure the availability
    // of RAIDPARTS-1 sources based on ongoing reads and stored readahead data.
    for (int i = RAIDPARTS; i--; )
    {
        // compile boundaries of data chunks that have been or are being fetched
        // (we do not record the beginning, as position 0 is implicitly valid)
        // a) already read data in the PartReq buffer
        chunks[SECTORFLOOR(basepos+rr->partpos[i])]--;

        // b) ongoing fetches on *other* channels
        if (i != part)
        {
            if (rr->fetcher[i].rem)
            {
                chunks[SECTORFLOOR(rr->fetcher[i].pos)]++;
                chunks[SECTORFLOOR(rr->fetcher[i].pos+rr->fetcher[i].rem)]--;
            }
        }

        // c) existing readahead data
        auto it = rr->fetcher[i].readahead.begin();
        auto end = rr->fetcher[i].readahead.end();

        while (it != end)
        {
            m_off_t t = it->first;

            // we mark our own readahead as always valid to prevent double reads
            char delta = 1;
            if (i == part) delta += OWNREADAHEAD;

            chunks[SECTORFLOOR(t)] += delta;

            // (concatenate contiguous readahead chunks to reduce chunks inserts)
            do {
                t += it->second.second;
            }  while (++it != end && t == it->first);

            chunks[SECTORFLOOR(t)] -= delta;
        }
    }

    // find range from the first position after the current read position
    // with less than RAIDPARTS-1 valid sources
    // (where we need to start fetching) to the first position thereafter with
    // RAIDPARTS-1 valid sources, if any (where we would need to stop fetching)
    char valid = RAIDPARTS;
    m_off_t startpos = -1, endpos = -1;
    std::cout << "Setposrem 2 (startpos=-1, endpos=-1, valid=6) [part = " << std::to_string(part) << "] Iterate over chunks -> chunks.size = " << chunks.size() << ", basepos="<<basepos<<", curpos=" << curpos << ", rr->partpos["<<(int)part<<"]=" << rr->partpos[(int)part] << "]" << std::endl;

    m_off_t dynamicReadahead = std::max((m_off_t)(((sourcesize*RAIDSECTOR)/(RAIDSECTOR*(RAIDPARTS-1)))), READAHEAD);
    for (auto it = chunks.begin(); it != chunks.end(); )
    {
        auto next_it = it;
        next_it++;

        std::cout << "Setposrem 2 (startpos=-1, endpos=-1) [part = " << std::to_string(part) << "] [for] valid(="<<(int)valid<<") += it->second(="<<(int)it->second<<") -> " << ((int)valid+it->second) << " [it->first = " << it->first << "]" << std::endl;
        valid += it->second;

        assert(valid >= 0 && VALIDPARTS(valid) < RAIDPARTS);

        if (startpos == -1)
        {
            std::cout << "Setposrem [part = " << std::to_string(part) << "] (startpos == -1) -> no startpos yet (our own readahead is excluded by valid being bumped by OWNREADAHEAD) [valid="<<(int)valid<<"]" << std::endl;
            // no startpos yet (our own readahead is excluded by valid being bumped by OWNREADAHEAD)
            if (valid < RAIDPARTS-1)
            {
                if (curpos < it->first)
                {
                    std::cout << "Setposrem [part = " << std::to_string(part) << "] [valid < RAIDPARTS-1] (curpos < it->first) -> startpos = it->first = " << it->first << " [basepos="<<basepos<<", curpos=" << curpos << ", rr->partpos["<<(int)part<<"]=" << rr->partpos[(int)part] << "] [valid="<<(int)valid<<"]" << std::endl;
                    startpos = it->first;
                }
                else if (next_it == chunks.end() || curpos < next_it->first)
                {
                    std::cout << "Setposrem [part = " << std::to_string(part) << "] [valid < RAIDPARTS-1] (curpos >= it->first) && (next_it == chunks.end() || curpos < next_it->first) -> startpos = curpos = " << curpos << " [basepos="<<basepos<<", curpos=" << curpos << ", rr->partpos["<<(int)part<<"]=" << rr->partpos[(int)part] << "] [valid="<<(int)valid<<"]" << std::endl;
                    startpos = curpos;
                }
            }
            else std::cout << "Setposrem [part = " << std::to_string(part) << "] (valid >= RAIDPARTS-1) -> nothing ... :( !! " << std::endl;
        }
        else
        {

            std::cout << "Setposrem [part = " << std::to_string(part) << "] (startpos != -1) -> it->first-startpos > READAHEAD) -> startpos valid, look for suitable endpos [valid="<<(int)valid<<"]" << std::endl;
            // startpos valid, look for suitable endpos
            // (must not cross own readahead data or any already sufficient raidparts)
            if (valid >= RAIDPARTS-1)
            {
                std::cout << "Setposrem [part = " << std::to_string(part) << "] (startpos != -1) (valid >= RAIDPARTS-1) -> endpos = it->first = " << it->first << " [valid="<<(int)valid<<"]" << std::endl;
                endpos = it->first;
                break;
            }
            else std::cout << "Setposrem [part = " << std::to_string(part) << "] (startpos != -1) (valid < RAIDPARTS-1) -> nothing .... :( [valid="<<(int)valid<<"]" << std::endl;
        }

        it = next_it;
    }

    // clear early, hoping that much of chunks is still in the L1/L2 cache
    chunks.clear();

    // there is always a startpos, even though it may be at sourcesize
    assert(startpos != -1);

    // we always resume fetching at a raidsector boundary.
    // (this may result in the unnecessary retransmission of up to 15 bytes
    // in case we resume on the same part.)
    startpos &= -RAIDSECTOR;

    std::cout << "Setposrem [part = " << std::to_string(part) << "] (startpos = " << startpos << ", endpos = " << endpos << ")  [basepos="<<basepos<<", curpos=" << curpos << "]" << std::endl;
    if (endpos == -1)
    {
        std::cout << "Setposrem [part = " << std::to_string(part) << "] (endpos == -1) -> no sufficient number of sources past startpos, we read to the end -> rem = rr->paddedpartsize-startpos = " << (rr->paddedpartsize-startpos) << " [rr->paddedpartsize="<<rr->paddedpartsize<<", startpos=" << startpos << "]" << std::endl;
        // no sufficient number of sources past startpos, we read to the end
        rem = rr->paddedpartsize-startpos;

    }
    else
    {
        std::cout << "Setposrem [part = " << std::to_string(part) << "] (endpos != -1) -> rem = endpos-startpos = "<<(endpos-startpos)<<" [endpos="<<endpos<<",startpos"<<startpos<<"]" << std::endl;
        assert(endpos >= startpos);
        rem = endpos-startpos;
    }

    std::cout << "Setposrem [part = " << std::to_string(part) << "] pos = startpos = " << startpos << "; setremfeed()  [basepos="<<basepos<<", curpos=" << curpos << "]" << std::endl;
    pos = startpos;

    setremfeed();
}

bool PartFetcher::setremfeed(unsigned numBytes)
{
    // request 1 less RAIDSECTOR as we will add up to 1 sector of 0 bytes at the end of the file - this leaves enough buffer space in the buffer pased to procdata for us to write past the reported length
    remfeed = numBytes ? std::min(static_cast<unsigned>(rem), numBytes) : rem;
    if (sourcesize-pos < remfeed)
    {
        if (sourcesize-pos >= 0)
        {
            remfeed = sourcesize-pos; // we only read to the physical end of the part
        }
        else
        {
            rem = 0;
            remfeed = 0;
        }
    }
    std::cout << "setremfeed [part = " << std::to_string(part) << "] [numBytes="<<numBytes<<"] Ojo -> changing remfeed. New remfeed = " << remfeed << std::endl;
    return remfeed != 0;
}

bool RaidReq::allconnected(int excludedPart)
{
    for (int i = RAIDPARTS; i--; ) if (i != excludedPart && !fetcher[i].connected) return false;

    return true;
}

m_off_t PartFetcher::getSocketSpeed()
{
    if (!timeInflight)
    {
        return 0;
    }
    // In Bytes per millisec
    return reqBytesReceived / timeInflight;
}

// close socket
void PartFetcher::closesocket(bool reuseSocket)
{
    std::cout << "closesocket [part=" << std::to_string(part) << ", disconnect="<<!reuseSocket<<"] [httpReq = " << rr->httpReqs[part] << ", rr = " << rr << "]" << std::endl;
    if (skip_setposrem)
    {
        skip_setposrem = false;
    }
    else
    {
        rem = 0;
        remfeed = 0;    // need to clear remfeed so that the disconnected channel does not corrupt feedlag
        std::cout << "Ojo -> changing remfeed. New remfeed = " << remfeed << std::endl;
    }
    postCompleted = false;
    if (inbuf) inbuf.reset(nullptr);

    if (connected)
    {
        auto& httpReq = rr->httpReqs[part];
        if (httpReq)
        {
            if (!reuseSocket || httpReq->status == REQ_INFLIGHT)
            {
                rr->cloudRaid->disconnect(httpReq);
            }
            rr->pool.socketrrs.del(httpReq);
            httpReq->status = REQ_READY;
        }
        connected = false;
    }
    else if (rr->httpReqs[part])
    {
        RaidReq* rrInMap = nullptr;
        if (rrInMap = rr->pool.socketrrs.lookup(rr->httpReqs[part]))
        {
            std::cout << "closesocket ALERT WTF!!!!!! [part=" << std::to_string(part) << "] -> !connected && httpReq IS IN SOCKETRRS !!!!!!!!] [httpReq = " << rr->httpReqs[part] << ", rr = " << rr << ", rr2 = " << rrInMap << "]" << std::endl;
            rr->pool.socketrrs.del(rr->httpReqs[part]);
        }
    }
}

// (re)create, set up socket and start (optionally delayed) io on it
int PartFetcher::trigger(raidTime delay, bool disconnect)
{
    std::cout << "[PartFetcher::trigger] part = " << std::to_string(part) << ", delay = " << delay << ", disconnect = " << disconnect << " [pos="<<pos<<", rem="<<rem<<", remfeed="<<remfeed<<", sourcesize="<<sourcesize<<", rr->paddedpartsize="<<rr->paddedpartsize<<"] [finished = " << finished << ", rr = " << rr << "]" << std::endl;
    if (delay == MAX_DELAY_IN_SECONDS)
    {
        rr->cloudRaid->onTransferFailure();
        return -1;
    }
    assert(!url.empty());

    if (finished)
    {
        closesocket();
        return -1;
    }

    if (disconnect)
    {
        if (rr->httpReqs[(int)part]->status == REQ_SUCCESS)
        {
            rem = 0;
            remfeed = 0;
        }
        else closesocket(true);
    }

    if (!rem)
    {
        if (pos == rr->paddedpartsize) std::cout << "[PartFetcher::trigger] (!rem) && (pos == rr->paddedpartsize) -> closesocket && return -1; part = " << std::to_string(part) << ", delay = " << delay << ", disconnect = " << disconnect << " [pos="<<pos<<", rem="<<rem<<", remfeed="<<remfeed<<", sourcesize="<<sourcesize<<", rr->paddedpartsize="<<rr->paddedpartsize<<"]" << std::endl;
        assert(pos <= rr->paddedpartsize);
        if (pos == rr->paddedpartsize)
        {
            closesocket();
            return -1;
        }
    }

    directTrigger(!delay);

    if (delay) delayuntil = currtime+delay;

    return delay;
}

bool PartFetcher::directTrigger(bool addDirectio)
{
    std::cout << "PartFetcher part = " << std::to_string(part) << ", directTrigger(addDirectio="<<addDirectio<<")" << std::endl;

    auto httpReq = rr->httpReqs[part];

    assert(httpReq != nullptr);
    assert(!connected || (rr->pool.socketrrs.lookup(httpReq) == nullptr));
    if (!connected)
    {
        rr->pool.socketrrs.set(httpReq, rr); // set up for event to be handled immediately on the wait thread
    }
    if (addDirectio && rr->pool.addDirectio(httpReq))
    {
        return true;
    }
    return !addDirectio;
}

// perform I/O on socket (which is assumed to exist)
int PartFetcher::io()
{
    std::cout << "[PartFetcher::io] part = " << std::to_string(part) << " [currtime="<<currtime<<", megaTime="<<Waiter::ds<<"] [finished = " << finished << "] [postCompleted="<<postCompleted<<"]" << std::to_string(part) << " [finished = " << finished << ", rr = " << rr << "]" << std::endl;

    if (currtime < delayuntil) std::cout << "[PartFetcher::io] part = " << std::to_string(part) << " currtime < delayuntil -> return -1 [currtime = " << currtime << ", delayuntil = " << delayuntil << "]" << std::endl;
    // prevent spurious epoll events from triggering a delayed reconnect early
    if (finished && rr->completed < NUMLINES && (rr->rem > rr->completed*RAIDLINE-rr->skip))
    {
        std::cout << "[PartFetcher::io] part = " << std::to_string(part) << " ALERT (completed < NUMLINES) -> rr->procreadahead() [completed = " << rr->completed << ", NUMLINES = " << NUMLINES << "] [finished = " << finished << "]" << std::endl;
        rr->procreadahead();
    }
    if ((currtime < delayuntil) || finished) return -1;

    auto httpReq = rr->httpReqs[part];
    assert(httpReq != nullptr);

std::cout << "[PartFetcher::io] part = " << std::to_string(part) << ", httpReq="<<httpReq<<", httpReq->status = " << httpReq->status << ", connected = " << connected << ", httpReq->httpstatus = " << httpReq->httpstatus << " [pos="<<pos<<", rem="<<rem<<", remfeed="<<remfeed<<", sourcesize="<<sourcesize<<", rr->paddedpartsize="<<rr->paddedpartsize<<"] [readahead.size = " << readahead.size() << "]" << std::endl;

    if (httpReq->status == REQ_FAILURE)
    {
        return onFailure();
    }
    else if (rr->allconnected((int)part))
    {
        std::cout << "[PartFetcher::io] part = " << std::to_string(part) << ", (rr->allconnected()) -> closesocket(true) -> we only need RAIDPARTS-1, so shut down the slowest one [pos="<<pos<<", rem="<<rem<<", remfeed="<<remfeed<<", sourcesize="<<sourcesize<<"]" << std::endl;
        // we only need RAIDPARTS-1 connections, so shut down the slowest one
        closesocket(true);
        return -1;
    }
    else if (httpReq->status == REQ_INFLIGHT)
    {
        directTrigger();
        return -1;
    }
    // unless the fetch position/length for the connection has been computed
    // before, we do so *after* the connection so that the order in which
    // the connections are established are the first criterion for slow source heuristics
    else if (!rem && httpReq->status != REQ_SUCCESS)
    {
        std::cout << "[PartFetcher::io] part = " << std::to_string(part) << ", (!rem) -> setposrem() && httpReq->status = " << httpReq->status << " [pos="<<pos<<", rem="<<rem<<", remfeed="<<remfeed<<", sourcesize="<<sourcesize<<"]" << std::endl;
        setposrem();
        if (httpReq->status == REQ_PREPARED) httpReq->status = REQ_READY;
        std::cout << "[PartFetcher::io] part = " << std::to_string(part) << ", (!rem) -> AFTER setposrem() && httpReq->status = " << httpReq->status << " -> [pos="<<pos<<", rem="<<rem<<", remfeed="<<remfeed<<", sourcesize="<<sourcesize<<"]" << std::endl;
    }

    if (httpReq->status == REQ_READY)
    {
        if (rem <= 0)
        {
            std::cout << "[PartFetcher::io] part = " << std::to_string(part) << ", (httpReq->status == REQ_READY) && (rem <= 0) -> closesocket && resumeall [pos="<<pos<<", rem="<<rem<<", remfeed="<<remfeed<<", sourcesize="<<sourcesize<<"]" << std::endl;
            closesocket();
            rr->resumeall((int)part);
            return -1;
        }

        if (postCompleted && (rr->processFeedLag() == (int)part))
        {
            std::cout << "[PartFetcher::io] part = " << std::to_string(part) << " ALERT: this part has been detected as lagged. return -1" << std::endl;
            return -1;
        }

        if (inbuf)
        {
            inbuf.reset(nullptr);
        }

        size_t npos = pos + rem;
        assert(npos <= rr->paddedpartsize);
        std::cout << "[PartFetcher::io] [part=" << std::to_string(part) << "] (REQ_READY) -> rr->cloudRaid->prepareRequest(httpReq=" << httpReq << ", url='"<</*<<url<<"*/"', pos=" << pos << ", npos=" << npos << ") size = " << (npos-pos) << " [sourcesize = " << sourcesize << ", rr->paddedpartsize = " << rr->paddedpartsize << ", reqBytesReceived = " << reqBytesReceived << "] [rem=" << rem << ", rr->completed=" << rr->completed << "] [partStartPos = " << partStartPos << ", maxRequestSize = " << rr->maxRequestSize << ", numPartsUnfinished = " << rr->numPartsUnfinished() << "] [feedlag = " << rr->feedlag[part] << "]" << std::endl;
        rr->cloudRaid->prepareRequest(httpReq, url, pos + partStartPos, npos + partStartPos);
        assert(httpReq->status == REQ_PREPARED);
        connected = true;
    }

    if (connected)
    {
        if (httpReq->status == REQ_PREPARED)
        {
            bool postDone = rr->cloudRaid->post(httpReq);
            if (postDone)
            {
                lastconnect = currtime;
                lastdata = currtime;
                postStartTime = std::chrono::system_clock::now();
            }
            else
            {
                return onFailure();
            }
        }

        if (httpReq->status == REQ_SUCCESS)
        {
            assert(!inbuf || httpReq->buffer_released);
            if (!inbuf || !httpReq->buffer_released)
            {
                std::cout << "[io] [part="<<std::to_string(part)<<"] httpReq->status = REQ_SUCCESS (!inbuf || !httpReq->buffer_released) -> assert (pos + partStartPos == httpReq->dlpos) [pos="<<pos<<", partStartPos = " << partStartPos << ", pos + partStartPos = " << (pos + partStartPos) << ", httpReq->dlpos="<<httpReq->dlpos<<"] [httpReq="<<httpReq<<"]" << std::endl;
                assert((pos + partStartPos) == httpReq->dlpos);
                const auto& postEndTime = std::chrono::system_clock::now();
                auto reqTime = std::chrono::duration_cast<std::chrono::milliseconds>(postEndTime - postStartTime).count();
                timeInflight += reqTime;
                inbuf.reset(httpReq->release_buf());
                httpReq->buffer_released = true;
                reqBytesReceived += inbuf->datalen();
                postCompleted = true;
                rr->feedlag[(int)part] += (inbuf->datalen() ? (((inbuf->datalen() / reqTime)*1000)/1024) : 0); // KB/s
                std::cout << "[io] [part="<<std::to_string(part)<<"] httpReq->status = REQ_SUCCESS -> reqTime = " << reqTime << " ms (req speed = " << (inbuf->datalen() ? (((inbuf->datalen() / reqTime)*1000)/1024) : 0) << " KB/s, req size = " << inbuf->datalen() << ") [prepareRequest]" << std::endl;
                rr->resumeall((int)part);
            }

std::cout << "[io] [part="<<std::to_string(part)<<"] httpReq->status = REQ_SUCCESS [remfeed = " << remfeed << ", inbuf->datalen = " << inbuf->datalen() << ", readahead.size = " << readahead.size() << "]" << std::endl;

            while (remfeed && inbuf->datalen())
            {
                size_t bufSize = inbuf->datalen();
                if (remfeed < bufSize) bufSize = remfeed;

                globalBytesReceived += bufSize;  // atomically added
                remfeed -= bufSize;
                rem -= bufSize;

                // completed a read: reset consecutive_errors
                if (!rem && consecutive_errors) consecutive_errors = 0;

                rr->procdata(part, inbuf->datastart(), pos, bufSize);

                if (!connected)
                {
                    std::cout << "[io] [part="<<std::to_string(part)<<"] !connected -> break" << std::endl;
                    break;
                }

                pos += bufSize;
                inbuf->start += bufSize;
            }

            lastdata = currtime;

            if (!remfeed && pos == sourcesize && sourcesize < rr->paddedpartsize)
            {
                // we have reached the end of a part requires padding
                static byte nulpad[RAIDSECTOR];

                rr->procdata(part, nulpad, pos, rr->paddedpartsize-sourcesize);
                rem = 0;
                pos = rr->paddedpartsize;
            }

            rr->procreadahead();

            if (!rem)
            {
                if (pos == rr->paddedpartsize)
                {
                    std::cout << "[io] [part="<<std::to_string(part)<<"] (!rem && pos == rr->paddedpartsize) -> closesocket() && finished = true && rr->resumeall() [rem = " << rem << ", remfeed = " << remfeed << ", inbuf->datalen = " << inbuf->datalen() << "] [readahead.size = " << readahead.size() << "] [rr->completed = " << rr->completed << "]" << std::endl;
                    closesocket();
                    finished = true;
                    return -1;
                }
            }
            else if (inbuf->datalen() == 0)
            {
                std::cout << "[io] [part="<<std::to_string(part)<<"] END REQ_SUCCESS -> (inbuf->datalen() == 0) -> inbuf.reset(nullptr)" << std::endl;
                inbuf.reset(nullptr);
                httpReq->status = REQ_READY;
            }
            else
            {
                setremfeed(std::min(static_cast<unsigned>(NUMLINES*RAIDSECTOR), static_cast<unsigned>(inbuf->datalen())));
            }
        }
        assert(httpReq->status == REQ_READY || httpReq->status == REQ_INFLIGHT || httpReq->status == REQ_SUCCESS || httpReq->status == REQ_FAILURE || httpReq->status == REQ_PREPARED);
        if (httpReq->status == REQ_FAILURE)
        {
            std::cout << "[io] [part="<<std::to_string(part)<<"] onFailure()" << std::endl;
            return onFailure();
        }

        if (httpReq->status == REQ_INFLIGHT)
        {
            directTrigger();
        }
        else
        {
            trigger();
        }
    }
    return -1;
}

int PartFetcher::onFailure()
{
    auto& httpReq = rr->httpReqs[part];
    assert(httpReq != nullptr);
    std::cout << "[PartFetcher::onFailure] httpReq->status = " << httpReq->status << ", httpReq->httpstatus = " << httpReq->httpstatus << std::endl;
    if (httpReq->status == REQ_FAILURE)
    {
        raidTime backoff = 0;
        {
            if (rr->cloudRaid->onRequestFailure(httpReq, part, backoff))
            {
                std::cout << "[PartFetcher::onFailure] After onRequestFailure -> new httpReq->status = " << httpReq->status << ", httpReq->httpstatus = " << httpReq->httpstatus << ", backoff = " << backoff << "" << std::endl;
                assert(!backoff || httpReq->status == REQ_PREPARED);
                if (httpReq->status == REQ_PREPARED)
                {
                    return trigger(backoff);
                }
                else if (httpReq->status == REQ_FAILURE)
                {
                    if (httpReq->httpstatus == 0)
                    {
                        httpReq->status = REQ_READY;
                    }
                    else
                    {
                        std::cout << "[PartFetcher::onFailure] probable transfer->failed()" << std::endl;
                        closesocket();
                        return -1;
                    }
                }
            }
        }

        static raidTime lastlog;
        if (currtime > lastlog)
        {
            LOG_warn << "CloudRAID connection to part " << part << " failed. Http status: " << httpReq->httpstatus;
            lastlog = currtime;
        }
        errors++;

        if (consecutive_errors > MAXRETRIES || httpReq->status == REQ_READY)
        {
            std::cout << "[PartFetcher::onFailure] (consecutive_errors > MAXRETRIES || httpReq->status == REQ_READY) closesocket() [part="<<std::to_string(part)<<"]" << std::endl;
            closesocket();
            for (int i = RAIDPARTS; i--;)
            {
                std::cout << "[PartFetcher::onFailure] (consecutive_errors > MAXRETRIES || httpReq->status == REQ_READY) i = " << i << ", rr->fetcher["<<i<<"].connected = " << rr->fetcher[i].connected << "" << std::endl;
                if (i != part && !rr->fetcher[i].connected)
                {
                    rr->fetcher[i].trigger();
                }
            }
            return -1;
        }
        else
        {
            std::cout << "[PartFetcher::onFailure] (consecutive_errors <= MAXRETRIES && httpReq->status != REQ_READY) for ... rr->fetcher[i].trigger() && trigger(backoff) [part="<<std::to_string(part)<<"]" << std::endl;
            consecutive_errors++;
            for (int i = RAIDPARTS; i--;)
            {
                if (i != part && !rr->fetcher[i].connected)
                {
                    rr->fetcher[i].trigger();
                }
            }

            if (httpReq->status == REQ_FAILURE) // shouldn't be
            {
                httpReq->status = REQ_READY;
            }
            return trigger(backoff);
        }
    }
    else
    {
        httpReq->status = REQ_READY;
        resume();
    }
    return -1;
}

// request a further chunk of data from the open connection
// (we cannot call io() directly due procdata() being non-reentrant)
void PartFetcher::cont(int numbytes)
{
    std::cout << "PartFetcher::cont(numbytes="<<numbytes<<") part = " << std::to_string(part) << "] [pos="<<pos<<", rr->paddedpartsize="<<rr->paddedpartsize<<"]" << std::endl;
    if (connected && pos < rr->paddedpartsize)
    {
        assert(!finished);
        std::cout << "PartFetcher::cont(numbytes="<<numbytes<<") (pos < rr->paddedpartsize) -> Ojo PREV -> changing remfeed. PREV remfeed = " << remfeed << std::endl;
        setremfeed(static_cast<unsigned>(numbytes));
        std::cout << "PartFetcher::cont(numbytes="<<numbytes<<") (pos < rr->paddedpartsize) -> Ojo -> changing remfeed. New remfeed = " << remfeed << std::endl;
        trigger();
    }
}

RaidReq::RaidReq(const Params& p, RaidReqPool& rrp, const std::shared_ptr<CloudRaid>& cloudRaid)
    : pool(rrp)
    , cloudRaid(cloudRaid)
{
    assert(p.tempUrls.size() > 0);
    assert((p.reqStartPos >= 0) && (p.reqlen <= p.filesize));
    httpReqs.resize(p.tempUrls.size());
    for(auto& httpReq : httpReqs)
    {
        httpReq = std::make_shared<HttpReqType>();
    }
    skip = 0;
    dataline = 0;
    reqStartPos = p.reqStartPos;
    std::cout << "[RaidReq::RaidReq] partStartPos = " << reqStartPos << ", MOD RAIDSECTOR(16) = " << (reqStartPos % RAIDSECTOR) << " MOD RAIDLINE(80) = " << (reqStartPos % RAIDLINE) << " [estimated partStartPos = " << (reqStartPos/(RAIDPARTS-1)) << "]" << std::endl;
    assert(reqStartPos % RAIDSECTOR == 0);
    assert(reqStartPos % RAIDLINE == 0);
    rem = p.reqlen;

    memset(partpos, 0, sizeof partpos);
    memset(feedlag, 0, sizeof feedlag);
    lagrounds = 0;

    memset(invalid, (1 << RAIDPARTS)-1, sizeof invalid);
    completed = 0;

    filesize = p.filesize;
    paddedpartsize = (raidPartSize(0, filesize)+RAIDSECTOR-1) & -RAIDSECTOR;
    maxRequestSize = p.maxRequestSize;

    lastdata = currtime;
    haddata = false;
    reported = false;
    missingsource = false;

    downloadStartTime = std::chrono::system_clock::now();

    int firstExcluded = 5; // Todo: get the unused source from raid.cpp
    std::vector<int> partOrder = { 5, 4, 3, 2, 1, 0 };

    for (int i : partOrder)
    {
        if (!p.tempUrls[i].empty())
        {
            // we don't trigger I/O on unknown source servers (which shouldn't exist in normal ops anyway)
            if (fetcher[i].setsource(p.tempUrls[i], this, i))
            {
                // this kicks off I/O on that source
                //fetcher[i].trigger();
                if (i != firstExcluded) fetcher[i].trigger();
            }
        }
        else
        {
            missingsource = true;
        }
    }
}

RaidReq::~RaidReq()
{
    {
        std::cout << "[~RaidReq] rrp_lock -> let other operations end (BEGIN PRE rrp_lock) [this = " << this << "]" << std::endl;
        lock_guard<recursive_mutex> g(pool.rrp_lock); // Let other operations end
        std::cout << "[~RaidReq] rrp_lock -> let other operations end (END POST rrp_lock) [this = " << this << "]" << std::endl;
    }
    const auto& downloadEndTime = std::chrono::system_clock::now();
    auto downloadTime = std::chrono::duration_cast<std::chrono::milliseconds>(downloadEndTime - downloadStartTime).count();
    std::cout << "[~RaidReq] downloadTime = " << downloadTime << " ms, size = " << filesize << "" << " [speed = " << (((filesize / downloadTime) * 1000) / 1024) << " KB/s] [this = " << this << "] [thread_id = " << std::this_thread::get_id() << "]" << std::endl;
}

int RaidReq::numPartsUnfinished()
{
   int count = 0;
   for (int i = RAIDPARTS; i--;)
   {
        if (!fetcher[i].finished)
        {
            count++;
        }
   }
   return count;
}

// resume fetching on a parked source that has become eligible again
void PartFetcher::resume(bool forceSetPosRem)
{
    bool skipPartsPending = false;
    std::cout << "[PartFetcher::resume] part = " << std::to_string(part) << " [forceSetPosRem = " << forceSetPosRem << "] [rem = " << rem << ", remfeed = " << remfeed << "] [connected = " << connected << ", httpReq->status = " << rr->httpReqs[part]->status << ", finished = " << finished << "] [pos = " << pos << ", rr->paddedpartsize = " << rr->paddedpartsize << "]" << std::endl;
    bool resumeCondition = finished ? false : true;

    std::cout << "[PartFetcher::resume] part = " << std::to_string(part) << " resumeCondition = " << resumeCondition << "" << std::endl;

    if (resumeCondition)
    {
        if (forceSetPosRem || ((!connected || !rem) && (pos < rr->paddedpartsize)))
        {
            setposrem();
        }

        if (rem || (pos < rr->paddedpartsize) || rr->httpReqs[part]->status == REQ_SUCCESS)
        {
            trigger();
        }
    }
    std::cout << "[PartFetcher::resume] part = " << std::to_string(part) << " end" << std::endl;
}

// try to resume fetching on all sources
void RaidReq::resumeall(int excludedPart)
{
    std::cout << "[RaidReq::resumeall] BEGIN [excludedPart = " << excludedPart << "] [rem = " << rem << "]" << std::endl;
    lock_guard<recursive_mutex> g(rr_lock);
    if (rem)
    {
        {
            for (int i = RAIDPARTS; i--; )
            {
                if (i != excludedPart)
                {
                    if (fetcher[i].finished)
                    {
                        fetcher[i].directTrigger();
                    }
                    else if (fetcher[i].connected)
                    {
                        fetcher[i].resume();
                    }
                }
            }
        }
    }
    std::cout << "[RaidReq::resumeall] END [excludedPart = " << excludedPart << "]" << std::endl;
}

// feed suitable readahead data
bool PartFetcher::feedreadahead()
{
    if (readahead.empty()) return false;

    int total = readahead.size();
    int remaining = total;

    while (remaining)
    {
        auto it = readahead.begin();

        // make sure that we feed gaplessly
        if ((it->first < (rr->dataline+rr->completed)*RAIDSECTOR))
        {
            std::cout << "[PartFetcher::feedreadahead] -> ALERTTTT (it->first < (rr->dataline+rr->completed)*RAIDSECTOR) -> [it->first = " << it->first << ", (rr->dataline+rr->completed)*RAIDSECTOR) = " << ((rr->dataline+rr->completed)*RAIDSECTOR) << ", rr->dataline = " << rr->dataline << ", rr->completed = " << rr->completed << "] total = " << total << ", remaining = " << remaining << " [part = " << std::to_string(part) << "]" << std::endl;
        }
        assert(it->first >= (rr->dataline+rr->completed)*RAIDSECTOR);

        // we only take over from any source if we match the completed boundary precisely
        if (it->first == (rr->dataline+rr->completed)*RAIDSECTOR) rr->partpos[(int)part] = it->first-rr->dataline*RAIDSECTOR;

        // always continue at any position on our own source
        if (it->first != rr->dataline*RAIDSECTOR+rr->partpos[(int)part]) break;

        // we do not feed chunks that cannot even be processed in part (i.e. that start at or past the end of the buffer)
        if (it->first-rr->dataline*RAIDSECTOR >= NUMLINES*RAIDSECTOR) break;

        m_off_t p = it->first;
        byte* d = it->second.first;
        unsigned l = it->second.second;
        readahead.erase(it);

        //std::cout << "[PartFetcher::feedreadahead] -> rr->procdata(part, d, p, l) [part = " << std::to_string(part) << ", d = " << (void*)d << ", p = " << p << ", l = " << l << "]" << std::endl;
        rr->procdata(part, d, p, l);
        free(d);

        remaining--;
    }

    return total && total != remaining;
}

// feed relevant read-ahead data to procdata
// returns true if any data was processed
void RaidReq::procreadahead()
{
    bool fed;

    do {
        fed = false;

        for (int i = RAIDPARTS; i--; )
        {
            if (fetcher[i].feedreadahead()) fed = true;
        }
    } while (fed);
}

// procdata() handles input in any order/size and will push excess data to readahead
// data is assumed to be 0-padded to paddedpartsize at EOF
void RaidReq::procdata(int part, byte* ptr, m_off_t pos, int len)
{
    std::cout << "[RaidReq::procdata] begin" << std::endl;
    m_off_t basepos = dataline*RAIDSECTOR;

    // we never read backwards
    assert((pos & -RAIDSECTOR) >= (basepos+(partpos[part] & -RAIDSECTOR)));

    bool consecutive = pos == basepos+partpos[part];

    // is the data non-consecutive (i.e. a readahead), OR is it extending past the end of our buffer?
    if (!consecutive || pos+len > basepos+NUMLINES*RAIDSECTOR)
    {
        std::cout << "[RaidReq::procdata] (!consecutive || pos+len > basepos+NUMLINES*RAIDSECTOR) -> readahead [part = " << (int)part << ", ptr = " << (void*)ptr << ", pos = " << pos << ", len = " << len << ", consecutive (not pure read-ahead) = " << consecutive << "]" << std::endl;
        auto ahead_ptr = ptr;
        auto ahead_pos = pos;
        auto ahead_len = len;

        // if this is a consecutive feed, we store the overflowing part as readahead data
        // and process the non-overflowing part normally
        if (consecutive)
        {
            ahead_pos = basepos+NUMLINES*RAIDSECTOR;
            ahead_ptr = ptr+(ahead_pos-pos);
            ahead_len = len-(ahead_pos-pos);
        }

        // enqueue for future processing
        auto itReadAhead = fetcher[part].readahead.find(ahead_pos);
        if (itReadAhead == fetcher[part].readahead.end() || itReadAhead->second.second < ahead_len)
        {
            std::cout << "[RaidReq::procdata]  " << std::string((itReadAhead == fetcher[part].readahead.end()) ? "Allocating" : "ReAllocating") << " ReadAhead -> ahead_pos = " << ahead_pos << ", ahead_len = " << ahead_len << std::endl;
            byte* p = itReadAhead != fetcher[part].readahead.end() ? static_cast<byte*>(std::realloc(itReadAhead->second.first, ahead_len)) : static_cast<byte*>(malloc(ahead_len));
            memcpy(p, ahead_ptr, ahead_len);
            fetcher[part].readahead[ahead_pos] = pair<byte*, unsigned>(p, ahead_len);
        }
        else std::cout << "[RaidReq::procdata]  ALERT ReadAhead pair already exist -> WON'T BE INSERTED -> ahead_pos = " << ahead_pos << ", ahead_len = " << ahead_len << std::endl;
        // if this is a pure readahead, we're done
        if (!consecutive) return;

        len = ahead_pos-pos;
    }
    else std::cout << "[RaidReq::procdata] (consecutive && pos+len <= basepos+NUMLINES*RAIDSECTOR) -> NOT readahead [part = " << (int)part << ", ptr = " << (void*)ptr << ", pos = " << pos << ", len = " << len << ", consecutive = " << consecutive << "]" << std::endl;

    // non-readahead data must flow contiguously
    assert(pos == partpos[part]+dataline*RAIDSECTOR);

    partpos[part] += len;

    unsigned t = pos-dataline*RAIDSECTOR;

    // ascertain absence of overflow (also covers parity part)
    assert(t+len <= sizeof data/(RAIDPARTS-1));

    // set valid bit for every block that's been received in full
    char partmask = 1 << part;
    int until = (t+len)/RAIDSECTOR;
    for (int i = t/RAIDSECTOR; i < until; i++)
    {
        assert(invalid[i] & partmask);
        invalid[i] -= partmask;
    }

    // copy (partial) blocks to data or parity buf
    if (part)
    {
        part--;

        byte* ptr2 = ptr;
        int len2 = len;
        byte* target = data + part * RAIDSECTOR + (t / RAIDSECTOR) * RAIDLINE;
        int partialSector = t % RAIDSECTOR;
        if (partialSector != 0)
        {
            target += partialSector;
            auto sectorBytes = std::min<int>(len2, RAIDSECTOR - partialSector);
            memcpy(target, ptr2, sectorBytes);
            target += sectorBytes + RAIDSECTOR * (RAIDPARTS - 2);
            len2 -= sectorBytes;
            ptr2 += sectorBytes;
        }
        while (len2 >= RAIDSECTOR)
        {
            *(raidsector_t*)target = *(raidsector_t*)ptr2;
            target += RAIDSECTOR * (RAIDPARTS - 1);
            ptr2 += RAIDSECTOR;
            len2 -= RAIDSECTOR;
        }
        partialSector = len2;
        if (partialSector != 0)
        {
            memcpy(target, ptr2, partialSector);
        }
    }
    else
    {
        // store parity data for subsequent merging
        memcpy(parity+t, ptr, len);
    }

    // merge new consecutive completed RAID lines so they are ready to be sent, direct from the data[] array
    auto old_completed = completed;
    for (; completed < until; completed++)
    {
        unsigned char mask = invalid[completed];
        auto bitsSet = __builtin_popcount(mask);   // machine instruction for number of bits set

        assert(bitsSet);
        if (bitsSet > 1)
        {
            break;
        }
        else
        {
            if (!(mask & 1))
            {
                // parity involved in this line

                int index = __builtin_ctz(mask);  // counts least significant consecutive 0 bits (ie 0-based index of least significant 1 bit).  Windows equivalent is _bitScanForward
                if (index > 0 && index < RAIDLINE)
                {
                    auto sectors = (raidsector_t*)(data + RAIDLINE * completed);
                    raidsector_t& target = sectors[index - 1];

                    target = ((raidsector_t*)parity)[completed];
                    if (!(mask & (1 << 1))) target ^= sectors[0];  // this method requires source and target are both aligned to their size
                    if (!(mask & (1 << 2))) target ^= sectors[1];
                    if (!(mask & (1 << 3))) target ^= sectors[2];
                    if (!(mask & (1 << 4))) target ^= sectors[3];
                    if (!(mask & (1 << 5))) target ^= sectors[4];
                    assert(RAIDPARTS == 6);
                }
            }
        }
    }

    if (completed > old_completed)
    {
        lastdata = currtime;
    }

   std::cout << "[RaidReq::procdata] end [completed = " << completed << ", old_completed = " << old_completed << "]" << std::endl;
}

void RaidReq::shiftdata(m_off_t len)
{
    std::cout << "Shiftdata begin [len = " << len << "] [skip = " << skip << ", rem = " << rem << "]" << std::endl;
    skip += len;
    rem -= len;

    if (rem)
    {
        int shiftby = skip/RAIDLINE;

        completed -= shiftby;

        skip %= RAIDLINE;

        // we remove completed sectors/lines from the beginning of all state buffers
        int eobData = 0, eobAll = 0;
        for (int i = RAIDPARTS; i--; ) //if (i != inactive)
        {
            if (i > 0 && partpos[i] > eobData) eobData = partpos[i];
            if (partpos[i] > eobAll) eobAll = partpos[i];
        }
        eobData = (eobData + RAIDSECTOR - 1) / RAIDSECTOR;
        eobAll = (eobAll + RAIDSECTOR - 1) / RAIDSECTOR;

        if (eobData > shiftby)
        {
            memmove(data, data+shiftby*RAIDLINE, (eobData-shiftby)*RAIDLINE);
        }

        if (eobAll > shiftby)
        {
            memmove(invalid, invalid + shiftby, eobAll - shiftby);
            memset(invalid + eobAll - shiftby, (1 << RAIDPARTS)-1, shiftby);
        }
        else
        {
            memset(invalid, (1 << RAIDPARTS)-1, shiftby);
        }

        dataline += shiftby;
        shiftby *= RAIDSECTOR;

        if (partpos[0] > shiftby) memmove(parity, parity+shiftby, partpos[0]-shiftby);

        // shift partpos[] by the dataline increment and retrigger data flow
        for (int i = RAIDPARTS; i--; )
        {
            partpos[i] -= shiftby;

            if (partpos[i] < 0) partpos[i] = 0;
            else
            {
                if (!fetcher[i].finished)
                {
                    // request 1 less RAIDSECTOR as we will add up to 1 sector of 0 bytes at the end of the file - this leaves enough buffer space in the buffer passed to procdata for us to write past the reported length
                    fetcher[i].cont(NUMLINES*RAIDSECTOR-partpos[i]);
                }
            }
        }

        haddata = true;
        lastdata = currtime;
    }
    std::cout << "Shiftdata end [len = " << len << "] [skip = " << skip << ", rem = " << rem << "]" << std::endl;
}


int RaidReq::processFeedLag()
{
    std::cout << "RaidReq::processFeedLag() begin [lagrounds+1="<<(lagrounds+1)<<", numPartsUnfinished="<<numPartsUnfinished()<<"]" << std::endl;
    int laggedPart = RAIDPARTS;
    if (++lagrounds >= numPartsUnfinished())
    {
        // (dominance is defined as the ratio between fastest and slowest)
        int slowest = 0, fastest = 0;

        int i = RAIDPARTS;

        while (i --> 0 && (!fetcher[slowest].connected || !feedlag[slowest]))
        {
            slowest++;
            fastest++;
        }
        if (slowest == RAIDPARTS) // Cannot compare yet
        {
            return RAIDPARTS;
        }


        for (i = RAIDPARTS; --i;)
        {
            if (fetcher[i].connected && feedlag[i])
            {
                if (feedlag[i] < feedlag[slowest])
                    slowest = i;
                else if (feedlag[i] > feedlag[fastest])
                    fastest = i;
            }
        }

        std::cout << "ProcessFeedLag() -> slowest = " << slowest << " (feedlag = " << feedlag[slowest] << ", status = " << httpReqs[slowest]->status << "), fastest = " << fastest << " (feedlag = " << feedlag[fastest] << ", status = " << httpReqs[fastest]->status << ")" << std::endl;
        if (!missingsource && fetcher[slowest].connected && !fetcher[slowest].finished && httpReqs[slowest]->status != REQ_SUCCESS && ((fetcher[slowest].rem - fetcher[fastest].rem) > ((NUMLINES * RAIDSECTOR * LAGINTERVAL * 3) / 4) || (feedlag[fastest] * 4 > feedlag[slowest] * 5)))
        {
            std::cout << "Alert: Slow channel detected -> " << slowest << " [feedlag = " << feedlag[slowest] << "], fastest = " << fastest << "] [feedlag = " << feedlag[fastest] << "]" << std::endl;
            // slow channel detected
            {
                fetcher[slowest].errors++;

                // check if we have a fresh and idle channel left to try
                int fresh = RAIDPARTS - 1;
                while (fresh >= 0 && (fetcher[fresh].connected || fetcher[fresh].errors || fetcher[fresh].finished))
                {
                    fresh--;
                }
                if (fresh >= 0)
                {
                    std::cout << "Trying fresh channel " << fresh << "... [part = " << slowest << ", httpReq->status = " << httpReqs[slowest]->status << "]" << std::endl;
                    fetcher[slowest].closesocket();
                    fetcher[fresh].resume(true);
                    laggedPart = slowest;
                }
            }
        }

        if (laggedPart != RAIDPARTS)
        {
            memset(feedlag, 0, sizeof feedlag);
        }
        lagrounds = 0;
        std::cout << "RaidReq::processFeedLag() END" << std::endl;
    }
    return laggedPart;
}

void RaidReq::dispatchio(const HttpReqPtr& httpReq)
{
    // fast lookup of which PartFetcher to call from a single cache line
    // we don't check for httpReq not being found since we know sometimes it won't be when we closed a socket to a slower/nonresponding server
    for (int i = RAIDPARTS; i--; )
    {
        if (httpReqs[i] == httpReq)
        {
            int t = fetcher[i].io();

            if (t > 0)
            {
                // this is a relatively infrequent ocurrence, so we tolerate the overhead of a std::set insertion/erasure
                pool.addScheduledio(currtime+t, httpReqs[i]);
            }
            break;
        }
    }
}

// execute cont()-triggered io()s
// must be called under lock
void RaidReq::handlependingio()
{
    while (!pendingio.empty())
    {
        auto httpReq = pendingio.front();
        pendingio.pop_front();
        dispatchio(httpReq);
    }
}

// watchdog: resolve stuck connections
void RaidReq::watchdog()
{
    //std::cout << "Watchdog begin" << std::endl;
    if (missingsource) return;

    // check for a single fast source hanging
    int hanging = 0;
    int hangingsource = -1;
    int idlegoodsource = -1;
    int numconnected = 0;
    int numReady = 0;
    int numInflight = 0;
    int numSuccess = 0;
    int numFailure = 0;
    int numPrepared = 0;

    for (int i = RAIDPARTS; i--; )
    {
        if (fetcher[i].connected)
        {
            if (httpReqs[i]->status == REQ_INFLIGHT)
            {
                fetcher[i].lastdata = httpReqs[i]->lastdata;
                if (fetcher[i].remfeed && ((currtime - httpReqs[i]->lastdata > 100) || (currtime - fetcher[i].lastdata > 200)))
                {
                    hanging++;
                    hangingsource = i;
                    std::cout << "Watchdog -> ALERT hanging !!!!! hangingsource = " << hangingsource << " [currtime = " << currtime << ", httpReqs[i]->lastdata = " << httpReqs[i]->lastdata << "]" << " [this = " << this << "]" << std::endl;
                }

                numInflight++;
            }
            else if (httpReqs[i]->status == REQ_READY) numReady++;
            else if (httpReqs[i]->status == REQ_SUCCESS) numSuccess++;
            else if (httpReqs[i]->status == REQ_FAILURE) numFailure++;
            else if (httpReqs[i]->status == REQ_PREPARED) numPrepared++;

            numconnected++;
        }
        else if (!fetcher[i].finished && !fetcher[i].errors) idlegoodsource = i;
    }

    if (hanging)
    {
        std::cout << "Watchdog -> hanging = " << hanging << ", hangingsource = " << hangingsource << ", numconnected = " << numconnected << ", idlegoodsource = " << idlegoodsource << " [allconnected = " << allconnected() << "] [numReady = " << numReady << ", numPrepared = " << numPrepared << ", numInflight = " << numInflight << ", numSuccess = " << numSuccess << ", numFailure = " << numFailure << "] [numPartsUnfinished = " << numPartsUnfinished() << "] [this = " << this << "]" << std::endl;
        {
            if (idlegoodsource >= 0)
            {
                std::cout << "Watchdog Attempted remedy: Switching from " << hangingsource << " to previously unused " << idlegoodsource << std::endl;
                fetcher[hangingsource].errors++;
                fetcher[hangingsource].closesocket();
                if (fetcher[idlegoodsource].trigger() == -1)
                {
                    std::cout << "ALERT ALERT ALERT, previously unused " << idlegoodsource << " returned -1!!! switching back to " << hangingsource << " !!!!" << std::endl;
                    fetcher[hangingsource].trigger();
                }
                std::cout << "Watchdog return" << std::endl;
                return;
            }
            else std::cout << "Inactive connection potentially bad" << std::endl;
        }
    }
    //std::cout << "Watchdog end" << std::endl;
}

m_off_t RaidReq::readdata(byte* buf, m_off_t len)
{
    std::cout << "readdata begin [len = " << len << "] [completed = " << completed << "] [this = " << this << "]" << std::endl;

    lock_guard<recursive_mutex> g(rr_lock);
    //std::cout << "readdata post lock guard [len = " << len << "] [this = " << this << "]" << std::endl;

    watchdog();

    m_off_t lenCompleted = 0;
    int old_completed, new_completed;
    m_off_t t;
    do
    {
    if (completed < NUMLINES)
    {
        old_completed = completed;
        //std::cout << "readdata -> (completed < NUMLINES) -> call procreadahead() [this = " << this << "]" << std::endl;
        procreadahead();
    }
    else
    {
        old_completed = 0;
    }
    new_completed = completed;
    t = completed*RAIDLINE-skip;

    //std::cout << "readdata t = " << t << ", completed = " << completed << ", skip = " << skip << " [lenCompleted = " << lenCompleted << "] [len = " << len << "] [this = " << this << "]" << std::endl;

    if (t > 0)
    {
        if ((t + lenCompleted) > len) t = len - lenCompleted;
        memmove(buf+lenCompleted, data+skip, t);
        lenCompleted += t;

        shiftdata(t);
    }
    else
    {
        if (currtime-lastdata > 1000)
        {
            std::cout << "readdata (currtime-lastdata > 1000) -> stuck" << std::endl;
            if (currtime-lastdata > 6000)
            {
                std::cout << "readdata (currtime-lastdata > 6000) -> timed out" << std::endl;
                LOG_warn << "CloudRAID feed timed out [haddata = " << haddata << "]";
                return -1;
            }

            if (!reported)
            {
                std::cout << "readdata (currtime-lastdata > 1000) -> !reported -> feed stuck" << std::endl;
                reported = true;
                LOG_warn << "CloudRAID feed stuck [haddata = " << haddata << "]";
            }
        }
    }
    } while (new_completed > old_completed && t > 0 && lenCompleted < len);

    if (lenCompleted)
    {
        std::cout << "readdata lenCompleted -> resumeall [this = " << this << "]" << std::endl;
        resumeall();
    }

    std::cout << "readdata end [" << std::string(lenCompleted ? "POSITIVE" : "NEGATIVE") << "] -> return lenCompleted = " << lenCompleted << " [t = " << t << "] [len = " << len << "] [this = " << this << "]" << std::endl;
    return lenCompleted;
}

void RaidReq::disconnect()
{
    for (int i = 0; i < httpReqs.size(); i++)
    {
        httpReqs[i]->disconnect();
    }
}

bool RaidReqPool::addScheduledio(raidTime scheduledFor, const HttpReqPtr& req)
{
//std::cout << "addScheduledio. pre lock_guard<recursive_mutex> g(rrp_queuelock)" << std::endl;
    lock_guard<recursive_mutex> g(rrp_queuelock);
//std::cout << "addScheduledio. post lock_guard<recursive_mutex> g(rrp_queuelock)" << std::endl;
    auto it = directio_set.insert(req);
    if (it.second)
    {
        auto itScheduled = scheduledio.insert(std::make_pair(scheduledFor, req));
        return itScheduled.second;
    }
    else std::cout << "ALERTISIMA !!! SCHEDULEDIO YA EXISTE [req = " << req << "]" << std::endl;
    return false;
}

bool RaidReqPool::addDirectio(const HttpReqPtr& req)
{
    return addScheduledio(0, req);
}

void RaidReqPool::raidproxyiothread()
{
    /*
    std::array<unsigned long, 11> reqTypes{};
    const char * reqTypesStr[11] = {
        "READY", "PREPARED", "UPLOAD_PREPARED_BUT_WAIT", "ENCRYPTING", "DECRYPTING",
        "DECRYPTED", "INFLIGHT", "SUCCESS", "FAILURE", "DONE", "ASYNCIO"
    };
    std::map<HttpReqPtr, std::array<unsigned long, 11>> reqTypesMap;
    */
    directsocket_queue events;

    const auto& raidThreadStartTime = std::chrono::system_clock::now();
    while (isRunning.load())
    {
        std::this_thread::sleep_for(std::chrono::microseconds(1));

////std::cout << "io. 0-0. Pre. Scheduledio.size = " << scheduledio.size() << ", events.size = " << events.size() << std::endl;
        if (!isRunning.load()) break;

        {
////std::cout << "io. 1 pre lock_guard<recursive_mutex> g(rrp_queuelock)" << std::endl;
            lock_guard<recursive_mutex> g(rrp_queuelock); // for directIo
////std::cout << "io. 1 post lock_guard<recursive_mutex> g(rrp_queuelock)" << std::endl;

////std::cout << "itScheduled loop..." << std::endl;
            auto itScheduled = scheduledio.begin();
            while (itScheduled != scheduledio.end() && itScheduled->first <= currtime)
            {
                //reqTypes[itScheduled->second->status]++;
                //reqTypesMap[itScheduled->second][itScheduled->second->status]++;
                if (itScheduled->second->status == REQ_INFLIGHT)
                {
                    itScheduled++;
                }
                else
                {
                    directio_set.erase(itScheduled->second);
                    events.emplace_front(std::move(itScheduled->second));
                    itScheduled = scheduledio.erase(itScheduled);
                }
            }
            while (itScheduled != scheduledio.end() && itScheduled->first > currtime)
            {
                std::cout << "[RaidReqPool::raidproxyiothread] Scheduled request waiting. Remaining time... " << (itScheduled->first - currtime) << " ds [Req time: " << itScheduled->first << ", currtime = " << currtime << "]" << " [this = " << this << "]" << std::endl;
            }

        }
        if (!events.empty())
        {
            for (int j = 2; j--; )
            {
            auto itEvent = events.begin();
            while (isRunning.load() && itEvent != events.end())
            {
                {
//std::cout << "io. 2. pre lock_guard<recursive_mutex> g(rrp_lock) [events.size() = " << events.size() << "]" << std::endl;
                    lock_guard<recursive_mutex> g(rrp_lock); // this lock guarantees RaidReq will not be deleted between lookup and dispatch - locked for a while but only affects the main thread with new raidreqs being added or removed
//std::cout << "io. 2. post lock_guard<recursive_mutex> g(rrp_lock) [events.size() = " << events.size() << "] [this = " << this << "]" << std::endl;
                    const HttpReqPtr& httpReq = *itEvent;
                    RaidReq* rr;
                    if ((rr = socketrrs.lookup(httpReq)))  // retrieved under extremely brief lock.  RaidReqs can not be deleted until we unlock rrp_lock
                    {
//std::cout << "io. 2.2. PRE std::unique_lock<mutex> g(rr->rr_lock, std::defer_lock) [events.size() = " << events.size() << "] [httpReq = " << httpReq << ", rr = " << rr << "] [this = " << this << "]" << std::endl;
                        std::unique_lock<recursive_mutex> grr(rr->rr_lock, std::defer_lock);
//std::cout << "io. 2.2.1 POST std::unique_lock<mutex> g(rr->rr_lock, std::defer_lock) -> PRE (grr.try_lock()) [events.size() = " << events.size() << "] [httpReq = " << httpReq << ", rr = " << rr << "] [rr->rr_lock = " << &rr->rr_lock << ", ggr = " << &grr << "] [this = " << this << "]" << std::endl;
                        if (grr.try_lock())
                        {
//std::cout << "io. 2.2.2 POST grr.try_lock() pre-dispatchio [events.size() = " << events.size() << "] [this = " << this << "]" << std::endl;
                            rr->dispatchio(httpReq);
//std::cout << "io. 2.2.3 POST grr.try_lock() post-dispatchio [events.size() = " << events.size() << "] [this = " << this << "]" << std::endl;
                            itEvent = events.erase(itEvent);
                        }
                        else
                        {
//std::cout << "io. 2.2.4 POST grr.try_lock() itEvent++ [events.size() = " << events.size() << "] [this = " << this << "]" << std::endl;
//std::cout << "io. 3. itEvent++ [events.size() = " << events.size() << "]" << std::endl;
                            itEvent++;
                        }
                    }
                    else
                    {
//std::cout << "io. 2.2.5 !(rr = socketrrs.lookup(httpReq)) -> PRE itEvent = events.erase(itEvent) [events.size() = " << events.size() << "] [this = " << this << "]" << std::endl;
                        itEvent = events.erase(itEvent);
//std::cout << "io. 2.2.6 !(rr = socketrrs.lookup(httpReq)) -> POST itEvent = events.erase(itEvent) [events.size() = " << events.size() << "] [this = " << this << "]" << std::endl;
                    }
                }
            }
            if (events.empty()) break;
            }
            while (!events.empty())
            {
                addDirectio(std::move((events.front())));
                events.pop_front();
            }
        }
//std::cout << "io. 0-2. Post 2. Scheduledio.size = " << scheduledio.size() << ", events.size = " << events.size() << " [this = " << this << "]" << std::endl;
    }
    const auto& raidThreadEndTime = std::chrono::system_clock::now();
    auto raidThreadTime = std::chrono::duration_cast<std::chrono::milliseconds>(raidThreadEndTime - raidThreadStartTime).count();
    std::cout << "[RaidReqPool::raidproxyiothread] END -> raidThreadTime = " << raidThreadTime << " ms [this = " << this << "]" << std::endl;
    /*
    std::cout << "[RaidReqPool::raidproxyiothread] END -> counting reqs [this = " << this << "]" << std::endl;
    unsigned long totalReqs = 0;
    for (size_t i = 0; i < 11; i++)
    {
        if (reqTypes[i]) totalReqs += reqTypes[i];
    }
    for (size_t i = 0; i < 11; i++)
    {
        if (reqTypes[i] != 0)
        {
            std::cout << "[REQ_" << reqTypesStr[i] << "] num = " << reqTypes[i] << " (" << (totalReqs ? ((reqTypes[i]*100)/totalReqs) : 0) << " %)" << std::endl;
        }

    }
    std::cout << "Desv: " << (totalReqs ? (((reqTypes[REQ_INFLIGHT] - reqTypes[REQ_SUCCESS])*100)/totalReqs) : 0) << std::endl;
    std::cout << "TOTAL: " << totalReqs << std::endl;
    std::cout << "\nCounting reqs per socket" << std::endl;
    unsigned long sInflight = 99999999999999;
    unsigned long bInflight = 0;
    unsigned long sSuccess = 99999999999999;
    unsigned long bSuccess = 0;
    for (auto& req : reqTypesMap)
    {
        std::cout << "\n=============== [REQUEST " << req.first << "] ===============" << std::endl;
        for (size_t i = 0; i < 11; i++)
        {
            if (req.second[i] != 0)
            {
                unsigned long percentage = ((req.second[i]*100)/reqTypes[i]);
                if ((percentage >= 5)) // Avoid almost-unused connection
                {
                    if (i == REQ_INFLIGHT && percentage < sInflight) sInflight = percentage;
                    if (i == REQ_INFLIGHT && percentage > bInflight) bInflight = percentage;
                    if (i == REQ_SUCCESS && percentage < sSuccess) sSuccess = percentage;
                    if (i == REQ_SUCCESS && percentage > bSuccess) bSuccess = percentage;
                }
                std::cout << "  [REQ_" << reqTypesStr[i] << "] num = " << req.second[i] << " (" << percentage << " %)" << std::endl;
            }

        }
    }
    unsigned long varInflight = bInflight ? bInflight - sInflight : 0;
    unsigned long varSuccess = bSuccess ? bSuccess - sSuccess : 0;
    std::cout << "========================================================\n" << std::endl;
    std::cout << "VARIANZA: (" << (varInflight+varSuccess) << ")" << std::endl;
    std::cout << "REQ_INFLIGHT: " << varInflight << std::endl;
    std::cout << "REQ_SUCCESS: " << varSuccess << std::endl;
    std::cout << raidThreadTime << " ms\n\n" << std::endl;

    std::cout << "IOTHREAD END" << std::endl;
    */
}

void RaidReqPool::raidproxyiothreadstart(RaidReqPool* rrp)
{
    rrp->raidproxyiothread();
}

RaidReqPool::RaidReqPool()
{
    std::cout << "[RaidReqPool::RaidReqPool] constructor call" << std::endl;
    isRunning.store(true);
    rrp_thread = std::thread(raidproxyiothreadstart, this);
}

RaidReqPool::~RaidReqPool()
{
    std::cout << "[RaidReqPool::~RaidReqPool] destructor call [this = " << this << "]" << std::endl;
    raidReq.reset();
    isRunning.store(false);
    std::cout << "[RaidReqPool::~RaidReqPool] destructor -> call rrpt_thread.join [this = " << this << "]" << std::endl;
    rrp_thread.join();
    std::cout << "[RaidReqPool::~RaidReqPool] destructor -> end [this = " << this << "]" << std::endl;
}

void RaidReqPool::request(const mega::SCCR::RaidReq::Params& p, const std::shared_ptr<CloudRaid>& cloudRaid)
{
    std::cout << "[RaidReqPool::request] PRE rrp_lock [this = " << this << "]" << std::endl;
    lock_guard<recursive_mutex> g(rrp_lock);
    std::cout << "[RaidReqPool::request] POST rrp_lock [this = " << this << "]" << std::endl;
    RaidReq* rr = new RaidReq(p, *this, cloudRaid);
    raidReq.reset(rr);
}
