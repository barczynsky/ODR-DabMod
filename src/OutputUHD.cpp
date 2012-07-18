/*
   Copyright (C) 2005, 2006, 2007, 2008, 2009, 2010 Her Majesty the
   Queen in Right of Canada (Communications Research Center Canada)

   Includes modifications for which no copyright is claimed
   2012, Matthias P. Braendli, matthias.braendli@mpb.li
 */
/*
   This file is part of CRC-DADMOD.

   CRC-DADMOD is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as
   published by the Free Software Foundation, either version 3 of the
   License, or (at your option) any later version.

   CRC-DADMOD is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with CRC-DADMOD.  If not, see <http://www.gnu.org/licenses/>.
 */

#define ENABLE_UHD 1

#include "OutputUHD.h"
#include "PcDebug.h"

#include <iostream>
#include <assert.h>
#include <stdexcept>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>

typedef std::complex<float> complexf;

OutputUHD::OutputUHD(char* device, unsigned sampleRate,
        double frequency, int txgain, bool enableSync, bool muteNoTimestamps) :
    ModOutput(ModFormat(1), ModFormat(0)),
    mySampleRate(sampleRate), myTxGain(txgain),
    enable_sync(enableSync),
    myFrequency(frequency), mute_no_timestamps(muteNoTimestamps)
{
    MDEBUG("OutputUHD::OutputUHD(device: %s) @ %p\n",
            device, this);

    myDevice = device;
    
#if ENABLE_UHD
    uhd::set_thread_priority_safe();

    //create a usrp device
    MDEBUG("OutputUHD:Creating the usrp device with: %s...\n",
            myDevice.c_str());
    myUsrp = uhd::usrp::multi_usrp::make(myDevice);
    MDEBUG("OutputUHD:Using device: %s...\n", myUsrp->get_pp_string().c_str());

    if (enable_sync) {
        MDEBUG("OutputUHD:Setting REFCLK and PPS input...\n");
        uhd::clock_config_t clock_config;
        clock_config.ref_source = uhd::clock_config_t::REF_SMA;
        clock_config.pps_source = uhd::clock_config_t::PPS_SMA;
        clock_config.pps_polarity = uhd::clock_config_t::PPS_POS;
        myUsrp->set_clock_config(clock_config, uhd::usrp::multi_usrp::ALL_MBOARDS);
    }

    std::cerr << "UHD clock source is " << 
            myUsrp->get_clock_source(0) << std::endl;

    std::cerr << "UHD time source is " <<
            myUsrp->get_time_source(0) << std::endl;

    //set the tx sample rate
    MDEBUG("OutputUHD:Setting rate to %d...\n", mySampleRate);
    myUsrp->set_tx_rate(mySampleRate);
    MDEBUG("OutputUHD:Actual TX Rate: %f Msps...\n", myUsrp->get_tx_rate());
    
    //set the centre frequency
    MDEBUG("OutputUHD:Setting freq to %f...\n", myFrequency);
    myUsrp->set_tx_freq(myFrequency);
    MDEBUG("OutputUHD:Actual frequency: %f\n", myUsrp->get_tx_freq());

    myUsrp->set_tx_gain(myTxGain);
    MDEBUG("OutputUHD:Actual TX Gain: %f ...\n", myUsrp->get_tx_gain());


    if (enable_sync) {
        /* handling time for synchronisation: wait until the next full
         * second, and set the USRP time at next PPS */
        struct timespec now;
        time_t seconds;
        if (clock_gettime(CLOCK_REALTIME, &now)) {
            fprintf(stderr, "errno: %d\n", errno);
            perror("OutputUHD:Error: could not get time: ");
        }
        else {
            seconds = now.tv_sec;

            MDEBUG("OutputUHD:sec+1: %ld ; now: %ld ...\n", seconds+1, now.tv_sec);
            while (seconds + 1 > now.tv_sec) {
                usleep(1);
                if (clock_gettime(CLOCK_REALTIME, &now)) {
                    fprintf(stderr, "errno: %d\n", errno);
                    perror("OutputUHD:Error: could not get time: ");
                    break;
                }
            }
            MDEBUG("OutputUHD:sec+1: %ld ; now: %ld ...\n", seconds+1, now.tv_sec);
            /* We are now shortly after the second change. */

            usleep(200000); // 200ms, we want the PPS to be later
            myUsrp->set_time_unknown_pps(uhd::time_spec_t(seconds + 2));
            fprintf(stderr, "OutputUHD: Setting USRP time next pps to %f\n",
                    uhd::time_spec_t(seconds + 2).get_real_secs());
        }

        usleep(1e6);
        fprintf(stderr, "OutputUHD: USRP time %f\n",
                myUsrp->get_time_now().get_real_secs());
    }
    
    
    // preparing output thread worker data
    uwd.myUsrp = myUsrp;
#else
    fprintf(stderr, "OutputUHD: UHD initialisation disabled at compile-time\n");
#endif

    uwd.frame0.ts.timestamp_valid = false;
    uwd.frame1.ts.timestamp_valid = false;
    uwd.sampleRate = mySampleRate;
    uwd.sourceContainsTimestamp = false;
    uwd.muteNoTimestamps = mute_no_timestamps;

    // Since we don't know the buffer size, we cannot initialise
    // the buffers here
    first_run = true;

    shared_ptr<barrier> b(new barrier(2));
    my_sync_barrier = b;
    uwd.sync_barrier = b;

    worker.start(&uwd);

    MDEBUG("OutputUHD:UHD ready.\n");
}


OutputUHD::~OutputUHD()
{
    MDEBUG("OutputUHD::~OutputUHD() @ %p\n", this);
    worker.stop();
}

int OutputUHD::process(Buffer* dataIn, Buffer* dataOut)
{
    struct frame_timestamp ts;

    // On the first call, we must do some allocation and we must fill
    // the first buffer
    // We will only wait on the barrier on the subsequent calls to 
    // OutputUHD::process
    if (first_run) {
        fprintf(stderr, "OutUHD.process:Initialising...\n");

        uwd.bufsize = dataIn->getLength();
        uwd.frame0.buf = malloc(uwd.bufsize);
        uwd.frame1.buf = malloc(uwd.bufsize);

        uwd.sourceContainsTimestamp = enable_sync && myEtiReader->sourceContainsTimestamp();

        // The worker begins by transmitting buf0
        memcpy(uwd.frame0.buf, dataIn->getData(), uwd.bufsize);

        myEtiReader->calculateTimestamp(ts);
        uwd.frame0.ts = ts;
        uwd.frame0.fct = myEtiReader->getFCT();

        activebuffer = 1;

        lastLen = uwd.bufsize;
        first_run = false;
        fprintf(stderr, "OutUHD.process:Initialising complete.\n");
    }
    else {

        if (lastLen != dataIn->getLength()) {
            // I expect that this never happens.
            fprintf(stderr,
                    "OutUHD.process:AAAAH PANIC input length changed from %zu to %zu !\n",
                    lastLen, dataIn->getLength());
            throw std::runtime_error("Non-constant input length!");
        }
        //fprintf(stderr, "OutUHD.process:Waiting for barrier\n");
        my_sync_barrier.get()->wait();
        
        // write into the our buffer while
        // the worker sends the other.

        myEtiReader->calculateTimestamp(ts);
        uwd.sourceContainsTimestamp = myEtiReader->sourceContainsTimestamp();

        if (activebuffer == 0) {
            memcpy(uwd.frame0.buf, dataIn->getData(), uwd.bufsize);

            uwd.frame0.ts = ts;
            uwd.frame0.fct = myEtiReader->getFCT();
        }
        else if (activebuffer == 1) {
            memcpy(uwd.frame1.buf, dataIn->getData(), uwd.bufsize);

            uwd.frame1.ts = ts;
            uwd.frame1.fct = myEtiReader->getFCT();
        }

        activebuffer = (activebuffer + 1) % 2;
    }

    return uwd.bufsize;

}

void UHDWorker::process(struct UHDWorkerData *uwd)
{
    int workerbuffer  = 0;
    time_t tx_second = 0;
    double pps_offset = 0;
    double last_pps   = 2.0;
    double usrp_time;

    //const struct timespec hundred_nano = {0, 100};

    size_t sizeIn;
    struct UHDWorkerFrameData* frame;

    size_t num_acc_samps; //number of accumulated samples
    int write_fail_count;

#if ENABLE_UHD
    // Transmit timeout
    const double timeout = 0.2;

    uhd::stream_args_t stream_args("fc32"); //complex floats
    uhd::tx_streamer::sptr myTxStream = uwd->myUsrp->get_tx_stream(stream_args);
    size_t bufsize = myTxStream->get_max_num_samps();
#endif

    const complexf* in;

    uhd::tx_metadata_t md;
    md.start_of_burst = false;
    md.end_of_burst = false;

    while (running) {
        md.has_time_spec = false;
        md.time_spec = uhd::time_spec_t(0.0);
        num_acc_samps = 0;
        write_fail_count = 0;

        /* Wait for barrier */
        // this wait will hopefully always be the second one
        // because modulation should be quicker than transmission
        //fprintf(stderr, "Worker:Waiting for barrier\n");
        uwd->sync_barrier.get()->wait();

        if (workerbuffer == 0) {
            frame = &(uwd->frame0);
        }
        else if (workerbuffer == 1) {
            frame = &(uwd->frame1);
        }
        else {
            fprintf(stderr, "UHDWorker.process: workerbuffer: %d\n", workerbuffer);
            perror("UHDWorker.process: workerbuffer is neither 0 nor 1!\n");
        }

        in = reinterpret_cast<const complexf*>(frame->buf);
        pps_offset = frame->ts.timestamp_pps_offset;
        //
        // Tx second from MNSC
        tx_second = frame->ts.timestamp_sec;

        sizeIn = uwd->bufsize / sizeof(complexf);
        
#if ENABLE_UHD
        // Check for ref_lock 
        if (! uwd->myUsrp->get_mboard_sensor("ref_locked", 0).to_bool()) {
            fprintf(stderr, "UHDWorker: RefLock lost !\n");
        }

        usrp_time = uwd->myUsrp->get_time_now().get_real_secs();
#else
        usrp_time = 0;
#endif

        if (uwd->sourceContainsTimestamp) {
            if (!frame->ts.timestamp_valid) {
                /* We have not received a full timestamp through
                 * MNSC. We sleep through the frame.
                 */
                fprintf(stderr, "UHDOut: Throwing sample %d away: incomplete timestamp %zu + %f\n",
                        frame->fct, tx_second, pps_offset);
                usleep(20000);
                goto loopend;
            }

            md.has_time_spec = true;
            md.time_spec = uhd::time_spec_t(tx_second, pps_offset);
            
            // md is defined, let's do some checks
            if (md.time_spec.get_real_secs() + 0.2 < usrp_time) {
                fprintf(stderr,
                    "* Timestamp in the past! offset: %f"
                    "  (%f) frame %d tx_second %zu; pps %f\n",
                    md.time_spec.get_real_secs() - usrp_time,
                    usrp_time, frame->fct, tx_second, pps_offset);
                goto loopend; //skip the frame
            }

#if ENABLE_UHD
            if (md.time_spec.get_real_secs() > usrp_time + TIMESTAMP_MARGIN_FUTURE) {
                fprintf(stderr,
                    "* Timestamp too far in the future! offset: %f\n",
                    md.time_spec.get_real_secs() - usrp_time);
                usleep(20000); //sleep so as to fill buffers
            }

            if (md.time_spec.get_real_secs() > usrp_time + TIMESTAMP_ABORT_FUTURE) {
                fprintf(stderr,
                    "* Timestamp way too far in the future! offset: %f\n",
                    md.time_spec.get_real_secs() - usrp_time);
                fprintf(stderr, "* Aborting\n");
                throw std::runtime_error("Timestamp error. Aborted.");
            }
#endif

            if (frame->fct % 50 < 4) {
                fprintf(stderr, "UHDOut (%f): frame %d tx_second %zu; pps %.9f\n",
                        usrp_time,
                        frame->fct, tx_second, pps_offset);
            }

        }
        else { // !uwd->sourceContainsTimestamp
            if (uwd->muteNoTimestamps) {
                /* There was some error decoding the timestamp
                 */
                fprintf(stderr, "UHDOut: Muting sample %d : no timestamp\n",
                        frame->fct);
                usleep(20000);
                goto loopend;
            }
        }

#if ENABLE_UHD
        PDEBUG("UHDWorker::process:max_num_samps: %zu.\n",
                myTxStream->get_max_num_samps());

        /*
        size_t num_tx_samps = myTxStream->send(
                dataIn, sizeIn, md, timeout);

        MDEBUG("UHDWorker::process:num_tx_samps: %zu.\n", num_tx_samps);
        */
        while (running && (num_acc_samps < sizeIn)) {
            size_t samps_to_send = std::min(sizeIn - num_acc_samps, bufsize);

            //ensure the the last packet has EOB set if the timestamps has been refreshed
            //and needs to be reconsidered.
            md.end_of_burst = (frame->ts.timestamp_refresh && (samps_to_send <= bufsize));

            //send a single packet
            size_t num_tx_samps = myTxStream->send(
                    &in[num_acc_samps],
                    samps_to_send, md, timeout);

            num_acc_samps += num_tx_samps;

            md.time_spec = uhd::time_spec_t(tx_second, pps_offset)
                         + uhd::time_spec_t(0, num_acc_samps/uwd->sampleRate);

            /*
            fprintf(stderr, "*** pps_offset %f, md.time_spec %f, usrp->now %f\n",
                            pps_offset,
                            md.time_spec.get_real_secs(),
                            uwd->myUsrp->get_time_now().get_real_secs());
            // */


            if (num_tx_samps == 0) {
#if 1
                fprintf(stderr,
                        "UHDWorker::process() unable to write to device, skipping frame!\n");
                break;
#else
                // This has been disabled, because if there is a write failure,
                // we'd better not insist and try to go on transmitting future
                // frames.
                // The goal is not to try to send by all means possible. It's 
                // more important to make sure the SFN is not disturbed.

                fprintf(stderr, "F");
                nanosleep(&hundred_nano, NULL);
                write_fail_count++;
                if (write_fail_count >= 3) {
                    double ts = md.time_spec.get_real_secs();
                    double t_usrp = uwd->myUsrp->get_time_now().get_real_secs();

                    fprintf(stderr, "*** USRP write fail count %d\n", write_fail_count);
                    fprintf(stderr, "*** delta %f, md.time_spec %f, usrp->now %f\n",
                            ts - t_usrp,
                            ts, t_usrp);

                    fprintf(stderr, "UHDWorker::process() unable to write to device, skipping frame!\n");
                    break;
                }
#endif
            }

            //std::cerr << std::endl << "Waiting for async burst ACK... " << std::flush;
            uhd::async_metadata_t async_md;
            if (uwd->myUsrp->get_device()->recv_async_msg(async_md, 0)) {
                std::string PREFIX = "### asyncronous UHD message : ";
                switch (async_md.event_code) {
                    case uhd::async_metadata_t::EVENT_CODE_BURST_ACK:
                        break;
                    case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW:
                        std::cerr << PREFIX << "Underflow" << std::endl;
                        break;
                    case uhd::async_metadata_t::EVENT_CODE_SEQ_ERROR:
                        std::cerr << PREFIX << "Packet loss between host and device." << std::endl;
                        break;
                    case uhd::async_metadata_t::EVENT_CODE_TIME_ERROR:
                        std::cerr << PREFIX << "Packet had time that was late." << std::endl;
                        break;
                    case uhd::async_metadata_t::EVENT_CODE_UNDERFLOW_IN_PACKET:
                        std::cerr << PREFIX << "Underflow occurred inside a packet." << std::endl;
                        break;
                    case uhd::async_metadata_t::EVENT_CODE_SEQ_ERROR_IN_BURST:
                        std::cerr << PREFIX << "Packet loss within a burst." << std::endl;
                        break;
                    default:
                        std::cerr << PREFIX << "unknown event code" << std::endl;
                        break;
                }
            }

            /*
            bool got_async_burst_ack = false;
            //loop through all messages for the ACK packet (may have underflow messages in queue)
            while (not got_async_burst_ack and uwd->myUsrp->get_device()->recv_async_msg(async_md, 0.2)){
                got_async_burst_ack = (async_md.event_code == uhd::async_metadata_t::EVENT_CODE_BURST_ACK);
            }
            //std::cerr << (got_async_burst_ack? "success" : "fail") << std::endl;
            // */


        }
#endif

        last_pps = pps_offset;

loopend:
        // swap buffers
        workerbuffer = (workerbuffer + 1) % 2;
    }
}