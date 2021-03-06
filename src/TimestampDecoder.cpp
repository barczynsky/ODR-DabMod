/*
   Copyright (C) 2005, 2006, 2007, 2008, 2009, 2010 Her Majesty the
   Queen in Right of Canada (Communications Research Center Canada)

   Includes modifications for which no copyright is claimed
   2012, Matthias P. Braendli, matthias.braendli@mpb.li
 */
/*
   This file is part of ODR-DabMod.

   ODR-DabMod is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as
   published by the Free Software Foundation, either version 3 of the
   License, or (at your option) any later version.

   ODR-DabMod is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with ODR-DabMod.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <queue>
#include <iostream>
#include <fstream>
#include <string>
#include <boost/lexical_cast.hpp>
#include <sys/types.h>
#include "PcDebug.h"
#include "TimestampDecoder.h"
#include "Eti.h"
#include "Log.h"

//#define MDEBUG(fmt, args...) fprintf (LOG, fmt , ## args) 
#define MDEBUG(fmt, args...) PDEBUG(fmt, ## args) 


void TimestampDecoder::calculateTimestamp(struct frame_timestamp& ts)
{
    struct frame_timestamp* ts_queued = new struct frame_timestamp;

    /* Push new timestamp into queue */
    ts_queued->timestamp_valid = full_timestamp_received_mnsc;
    ts_queued->timestamp_sec = time_secs;
    ts_queued->timestamp_pps_offset = time_pps;

    ts_queued->timestamp_refresh = offset_changed;
    offset_changed = false;

    *ts_queued += timestamp_offset;

    queue_timestamps.push(ts_queued);

    /* Here, the queue size is one more than the pipeline delay, because
     * we've just added a new element in the queue.
     *
     * Therefore, use <= and not < for comparison
     */
    if (queue_timestamps.size() <= modconfig.delay_calculation_pipeline_stages) {
        //fprintf(stderr, "* %zu %u ", queue_timestamps.size(), modconfig.delay_calculation_pipeline_stages);
        /* Return invalid timestamp until the queue is full */
        ts.timestamp_valid = false;
        ts.timestamp_sec = 0;
        ts.timestamp_pps_offset = 0;
        ts.timestamp_refresh = false;
    }
    else {
        //fprintf(stderr, ". %zu ", queue_timestamps.size());
        /* Return timestamp from queue */
        ts_queued = queue_timestamps.front();
        queue_timestamps.pop();
        /*fprintf(stderr, "ts_queued v:%d, sec:%d, pps:%f, ref:%d\n",
                ts_queued->timestamp_valid,
                ts_queued->timestamp_sec,
                ts_queued->timestamp_pps_offset,
                ts_queued->timestamp_refresh);*/
        ts = *ts_queued;
        /*fprintf(stderr, "ts v:%d, sec:%d, pps:%f, ref:%d\n\n",
                ts.timestamp_valid,
                ts.timestamp_sec,
                ts.timestamp_pps_offset,
                ts.timestamp_refresh);*/

        delete ts_queued;
    }

    PDEBUG("Timestamp queue size %zu, delay_calc %u\n",
            queue_timestamps.size(),
            modconfig.delay_calculation_pipeline_stages);

    if (queue_timestamps.size() > modconfig.delay_calculation_pipeline_stages) {
        myLogger.level(error) << "Error: Timestamp queue is too large : size " <<
            queue_timestamps.size() << "! This should not happen !";
    }

    //ts.print("calc2 ");
}

void TimestampDecoder::pushMNSCData(int framephase, uint16_t mnsc)
{
    struct eti_MNSC_TIME_0 *mnsc0;
    struct eti_MNSC_TIME_1 *mnsc1;
    struct eti_MNSC_TIME_2 *mnsc2;
    struct eti_MNSC_TIME_3 *mnsc3;

    switch (framephase)
    {
        case 0:
            mnsc0 = (struct eti_MNSC_TIME_0*)&mnsc;
            enableDecode = (mnsc0->type == 0) &&
                (mnsc0->identifier == 0);
            gmtime_r(0, &temp_time);
            break;

        case 1:
            mnsc1 = (struct eti_MNSC_TIME_1*)&mnsc;
            temp_time.tm_sec = mnsc1->second_tens * 10 + mnsc1->second_unit;
            temp_time.tm_min = mnsc1->minute_tens * 10 + mnsc1->minute_unit;

            if (!mnsc1->sync_to_frame)
            {
                enableDecode = false;
                PDEBUG("TimestampDecoder: MNSC time info is not synchronised to frame\n");
            }

            break;

        case 2:
            mnsc2 = (struct eti_MNSC_TIME_2*)&mnsc;
            temp_time.tm_hour = mnsc2->hour_tens * 10 + mnsc2->hour_unit;
            temp_time.tm_mday = mnsc2->day_tens * 10 + mnsc2->day_unit;
            break;

        case 3:
            mnsc3 = (struct eti_MNSC_TIME_3*)&mnsc;
            temp_time.tm_mon = (mnsc3->month_tens * 10 + mnsc3->month_unit) - 1;
            temp_time.tm_year = (mnsc3->year_tens * 10 + mnsc3->year_unit) + 100;

            if (enableDecode)
            {
                full_timestamp_received_mnsc = true;
                updateTimestampSeconds(mktime(&temp_time));
            }
            break;
    }

    MDEBUG("TimestampDecoder::pushMNSCData(%d, 0x%x)\n", framephase, mnsc);
    MDEBUG("                            -> %s\n", asctime(&temp_time));
    MDEBUG("                            -> %zu\n", mktime(&temp_time));
}

void TimestampDecoder::updateTimestampSeconds(uint32_t secs)
{
    MDEBUG("TimestampDecoder::updateTimestampSeconds(%d)\n", secs);
    if (inhibit_second_update > 0)
    {
        inhibit_second_update--;
    }
    else
    {
        time_secs = secs;
    }
}

void TimestampDecoder::updateTimestampPPS(double pps)
{
    MDEBUG("TimestampDecoder::updateTimestampPPS(%f)\n", pps);

    if (time_pps > pps) // Second boundary crossed
    {
        MDEBUG("TimestampDecoder::updateTimestampPPS crossed second\n");

        // The second for the next eight frames will not
        // be defined by the MNSC
        inhibit_second_update = 2;
        time_secs += 1;
    }

    time_pps = pps;

}

void TimestampDecoder::updateTimestampEti(int framephase, uint16_t mnsc, double pps)
{
    updateTimestampPPS(pps);
    pushMNSCData(framephase, mnsc);
}


bool TimestampDecoder::updateModulatorOffset()
{
    using namespace std;
    using boost::lexical_cast;
    using boost::bad_lexical_cast;

    if (modconfig.use_offset_fixed)
    {
        timestamp_offset = modconfig.offset_fixed;
        return true;
    }
    else if (modconfig.use_offset_file)
    {
        bool r = false;
        double newoffset;

        std::string filedata;
        ifstream filestream;

        try
        {
            filestream.open(modconfig.offset_filename.c_str());
            if (!filestream.eof())
            {
                getline(filestream, filedata);
                try
                {
                    newoffset = lexical_cast<double>(filedata);
                    r = true;
                }
                catch (bad_lexical_cast& e)
                {
                    myLogger.level(error) << "Error parsing timestamp offset from file '" << modconfig.offset_filename << "'";
                    r = false;
                }
            }
            else
            {
                myLogger.level(error) << "Error reading from timestamp offset file: eof reached\n";
                r = false;
            }
            filestream.close();
        }
        catch (exception& e)
        {
            myLogger.level(error) << "Error opening timestamp offset file\n";
            r = false;
        }


        if (r)
        {
            if (timestamp_offset != newoffset)
            {
                timestamp_offset = newoffset;
                myLogger.level(info) << "TimestampDecoder::updateTimestampOffset: new offset is " << timestamp_offset;
                offset_changed = true;
            }

        }

        return r;
    }
    else {
        return false;
    }
}
