#include "globals.h"
#include "decoders.h"
#include "ibm.h"
#include "crc.h"
#include "fluxmap.h"
#include "sector.h"
#include <string.h>

static_assert(std::is_trivially_copyable<IbmIdam>::value);

SectorVector AbstractIbmDecoder::decodeToSectors(const RawRecordVector& rawRecords)
{
    bool idamValid = false;
    IbmIdam idam;
    std::vector<std::unique_ptr<Sector>> sectors;

    for (auto& rawrecord : rawRecords)
    {
        const auto& bytes = decodeFmMfm(rawrecord->data);
        int headerSize = skipHeaderBytes();
        auto data = bytes.begin() + headerSize;
        unsigned len = bytes.size() - headerSize;

        switch (data[0])
        {
            case IBM_IAM:
                /* Track header. Ignore. */
                break;

            case IBM_IDAM:
            {
                if (len < sizeof(idam))
                    break;
                memcpy(&idam, &data[0], sizeof(idam));

				uint16_t crc = crc16(CCITT_POLY, &bytes[0], &bytes[offsetof(IbmIdam, crc) + headerSize]);
				uint16_t wantedCrc = (idam.crc[0]<<8) | idam.crc[1];
				idamValid = (crc == wantedCrc);
                break;
            }

            case IBM_DAM1:
            case IBM_DAM2:
            {
                if (!idamValid)
                    break;
                
                unsigned size = 1 << (idam.sectorSize + 7);
                if (size > (len + IBM_DAM_LEN + 2))
                    break;

                const uint8_t* userStart = &data[IBM_DAM_LEN];
                const uint8_t* userEnd = userStart + size;
				uint16_t crc = crc16(CCITT_POLY, &bytes[0], userEnd);
				uint16_t wantedCrc = (userEnd[0] << 8) | userEnd[1];
				int status = (crc == wantedCrc) ? Sector::OK : Sector::BAD_CHECKSUM;

                std::vector<uint8_t> sectordata(size);
                memcpy(&sectordata[0], userStart, size);

                int sectorNum = idam.sector - _sectorIdBase;
                auto sector = std::unique_ptr<Sector>(
					new Sector(status, idam.cylinder, idam.side, sectorNum, sectordata));
                sectors.push_back(std::move(sector));
                idamValid = false;
                break;
            }
        }
    }

    return sectors;
}

nanoseconds_t IbmMfmDecoder::guessClock(Fluxmap& fluxmap) const
{
    return fluxmap.guessClock() / 2;
}

int IbmFmDecoder::recordMatcher(uint64_t fifo) const
{
    /*
     * The markers at the beginning of records are special, and have
     * missing clock pulses, allowing them to be found by the logic.
     * 
     * IAM record:
     * flux:   XXXX-XXX-XXXX-X- = 0xf77a
     * clock:  X X - X - X X X  = 0xd7
     * data:    X X X X X X - - = 0xfc
     * 
     * ID record:
     * flux:   XXXX-X-X-XXXXXX- = 0xf57e
     * clock:  X X - - - X X X  = 0xc7
     * data:    X X X X X X X - = 0xfe
     * 
     * ID record:
     * flux:   XXXX-X-X-XX-XXXX = 0xf56f
     * clock:  X X - - - X X X  = 0xc7
     * data:    X X X X X - X X = 0xfe
     */
         
    uint16_t masked = fifo & 0xffff;
    if ((masked == 0xf77a) || (masked == 0xf57e) || (masked == 0xf56f))
        return 16;
    return 0;
}

int IbmMfmDecoder::recordMatcher(uint64_t fifo) const
{
    /* 
     * The IAM record, which is the first one on the disk (and is optional), uses
     * a distorted 0xC2 0xC2 0xC2 marker to identify it. Unfortunately, if this is
     * shifted out of phase, it becomes a legal encoding, so if we're looking at
     * real data we can't honour this. It can easily be read by keeping state as
     * to whether we're reading or seeking, but it's completely useless and so I
     * can't be bothered.
     * 
     * 0xC2 is:
     * data:    1  1  0  0  0  0  1 0
     * mfm:     01 01 00 10 10 10 01 00 = 0x5254
     * special: 01 01 00 10 00 10 01 00 = 0x5224
     *                    ^^^^
     * shifted: 10 10 01 00 01 00 10 0. = legal, and might happen in real data
     * 
     * Therefore, when we've read the marker, the input fifo will contain
     * 0xXXXX522252225222.
     * 
     * All other records use 0xA1 as a marker:
     * 
     * 0xA1  is:
     * data:    1  0  1  0  0  0  0  1
     * mfm:     01 00 01 00 10 10 10 01 = 0x44A9
     * special: 01 00 01 00 10 00 10 01 = 0x4489
     *                       ^^^^^
     * shifted: 10 00 10 01 00 01 00 1
     * 
     * When this is shifted out of phase, we get an illegal encoding (you
     * can't do 10 00). So, if we ever see 0x448944894489 in the input
     * fifo, we know we've landed at the beginning of a new record.
     */
         
    uint64_t masked = fifo & 0xffffffffffffLL;
    if (masked == 0x448944894489LL)
        return 48;
    return 0;
}
