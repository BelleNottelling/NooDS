/*
    Copyright 2019-2022 Hydr8gon

    This file is part of NooDS.

    NooDS is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    NooDS is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with NooDS. If not, see <https://www.gnu.org/licenses/>.
*/

#include <algorithm>

#include "wifi.h"
#include "core.h"

Wifi::Wifi(Core *core): core(core)
{
    // Set some default BB register values
    bbRegisters[0x00] = 0x6D;
    bbRegisters[0x5D] = 0x01;
    bbRegisters[0x64] = 0xFF;

    // Prepare tasks to be used with the scheduler
    processPacketsTask = std::bind(&Wifi::processPackets, this);
}

void Wifi::scheduleInit()
{
    // Schedule packet processing for as long as there are connections
    core->schedule(Task(&processPacketsTask, 1024));
    scheduled = true;
}

void Wifi::addConnection(Core *core)
{
    // Add an external core to this one's connection list
    mutex.lock();
    connections.push_back(&core->wifi);
    mutex.unlock();

    // Add this core to the external one's connection list
    core->wifi.mutex.lock();
    core->wifi.connections.push_back(this);
    core->wifi.mutex.unlock();
}

void Wifi::remConnection(Core *core)
{
    // Remove an external core from this one's connection list
    mutex.lock();
    auto position = std::find(connections.begin(), connections.end(), &core->wifi);
    connections.erase(position);
    mutex.unlock();

    // Remove this core from the external one's connection list
    core->wifi.mutex.lock();
    position = std::find(core->wifi.connections.begin(), core->wifi.connections.end(), this);
    core->wifi.connections.erase(position);
    core->wifi.mutex.unlock();
}

void Wifi::sendInterrupt(int bit)
{
    // Trigger a WiFi interrupt if W_IF & W_IE changes from zero
    if (!(wIe & wIrf) && (wIe & BIT(bit)))
        core->interpreter[1].sendInterrupt(24);

    wIrf |= BIT(bit);
}

void Wifi::processPackets()
{
    mutex.lock();

    // Write all queued packets to the circular buffer
    for (size_t i = 0; i < packets.size(); i++)
    {
        uint16_t size = (packets[i][4] + 12) / 2;

        for (size_t j = 0; j < size; j++)
        {
            // Write a half-word of the packet to memory
            core->memory.write<uint16_t>(1, 0x4804000 + (wRxbufWrcsr << 1), packets[i][j]);

            // Increment the circular buffer address
            if ((wRxbufBegin & 0x1FFE) != (wRxbufEnd & 0x1FFE))
            {
                wRxbufWrcsr++;
                wRxbufWrcsr = (wRxbufBegin & 0x1FFE) + (wRxbufWrcsr - (wRxbufBegin & 0x1FFE)) % ((wRxbufEnd & 0x1FFE) - (wRxbufBegin & 0x1FFE));
                wRxbufWrcsr &= 0x1FFF;
            }
        }

        delete[] packets[i];

        // Trigger a receive complete interrupt
        sendInterrupt(0);
    }

    // Reschedule the task as long as there are still connections
    if (connections.empty())
        scheduled = false;
    else
        core->schedule(Task(&processPacketsTask, 1024));

    packets.clear();
    mutex.unlock();
}

void Wifi::writeWModeWep(uint16_t mask, uint16_t value)
{
    // Write to the W_MODE_WEP register
    wModeWep = (wModeWep & ~mask) | (value & mask);
}

void Wifi::writeWIrf(uint16_t mask, uint16_t value)
{
    // Write to the W_IF register
    // Setting a bit actually clears it to acknowledge an interrupt
    wIrf &= ~(value & mask);
}

void Wifi::writeWIe(uint16_t mask, uint16_t value)
{
    // Trigger a WiFi interrupt if W_IF & W_IE changes from zero
    if (!(wIe & wIrf) && (value & mask & wIrf))
        core->interpreter[1].sendInterrupt(24);

    // Write to the W_IE register
    mask &= 0xFBFF;
    wIe = (wIe & ~mask) | (value & mask);
}

void Wifi::writeWMacaddr(int index, uint16_t mask, uint16_t value)
{
    // Write to one of the W_MACADDR registers
    wMacaddr[index] = (wMacaddr[index] & ~mask) | (value & mask);
}

void Wifi::writeWBssid(int index, uint16_t mask, uint16_t value)
{
    // Write to one of the W_BSSID registers
    wBssid[index] = (wBssid[index] & ~mask) | (value & mask);
}

void Wifi::writeWAidFull(uint16_t mask, uint16_t value)
{
    // Write to the W_AID_FULL register
    mask &= 0x07FF;
    wAidFull = (wAidFull & ~mask) | (value & mask);
}

void Wifi::writeWRxcnt(uint16_t mask, uint16_t value)
{
    // Write to the W_RXCNT register
    mask &= 0xFF0E;
    wRxcnt = (wRxcnt & ~mask) | (value & mask);

    // Latch W_RXBUF_WR_ADDR to W_RXBUF_WRCSR
    if (value & BIT(0))
        wRxbufWrcsr = wRxbufWrAddr;
}

void Wifi::writeWPowerstate(uint16_t mask, uint16_t value)
{
    // Write to the W_POWERSTATE register
    mask &= 0x0003;
    wPowerstate = (wPowerstate & ~mask) | (value & mask);

    // Set the power state to enabled if requested
    if (wPowerstate & BIT(1))
        wPowerstate &= ~BIT(9);
}

void Wifi::writeWPowerforce(uint16_t mask, uint16_t value)
{
    // Write to the W_POWERFORCE register
    mask &= 0x8001;
    wPowerforce = (wPowerforce & ~mask) | (value & mask);

    // Force set the power state if requested
    if (wPowerforce & BIT(15))
        wPowerstate = (wPowerstate & ~BIT(9)) | ((wPowerforce & BIT(0)) << 9);
}

void Wifi::writeWRxbufBegin(uint16_t mask, uint16_t value)
{
    // Write to the W_RXBUF_BEGIN register
    wRxbufBegin = (wRxbufBegin & ~mask) | (value & mask);
}

void Wifi::writeWRxbufEnd(uint16_t mask, uint16_t value)
{
    // Write to the W_RXBUF_END register
    wRxbufEnd = (wRxbufEnd & ~mask) | (value & mask);
}

void Wifi::writeWRxbufWrAddr(uint16_t mask, uint16_t value)
{
    // Write to the W_RXBUF_WR_ADDR register
    mask &= 0x0FFF;
    wRxbufWrAddr = (wRxbufWrAddr & ~mask) | (value & mask);
}

void Wifi::writeWRxbufRdAddr(uint16_t mask, uint16_t value)
{
    // Write to the W_RXBUF_RD_ADDR register
    mask &= 0x1FFE;
    wRxbufRdAddr = (wRxbufRdAddr & ~mask) | (value & mask);
}

void Wifi::writeWRxbufReadcsr(uint16_t mask, uint16_t value)
{
    // Write to the W_RXBUF_READCSR register
    mask &= 0x0FFF;
    wRxbufReadcsr = (wRxbufReadcsr & ~mask) | (value & mask);
}

void Wifi::writeWRxbufGap(uint16_t mask, uint16_t value)
{
    // Write to the W_RXBUF_GAP register
    mask &= 0x1FFE;
    wRxbufGap = (wRxbufGap & ~mask) | (value & mask);
}

void Wifi::writeWRxbufGapdisp(uint16_t mask, uint16_t value)
{
    // Write to the W_RXBUF_GAPDISP register
    mask &= 0x0FFF;
    wRxbufGapdisp = (wRxbufGapdisp & ~mask) | (value & mask);
}

void Wifi::writeWRxbufCount(uint16_t mask, uint16_t value)
{
    // Write to the W_RXBUF_COUNT register
    mask &= 0x0FFF;
    wRxbufCount = (wRxbufCount & ~mask) | (value & mask);
}

void Wifi::writeWTxbufWrAddr(uint16_t mask, uint16_t value)
{
    // Write to the W_TXBUF_WR_ADDR register
    mask &= 0x1FFE;
    wTxbufWrAddr = (wTxbufWrAddr & ~mask) | (value & mask);
}

void Wifi::writeWTxbufCount(uint16_t mask, uint16_t value)
{
    // Write to the W_TXBUF_COUNT register
    mask &= 0x0FFF;
    wTxbufCount = (wTxbufCount & ~mask) | (value & mask);
}

void Wifi::writeWTxbufWrData(uint16_t mask, uint16_t value)
{
    // Write a value to WiFi RAM
    core->memory.write<uint16_t>(1, 0x4804000 + wTxbufWrAddr, value & mask);

    // Increment the write address
    wTxbufWrAddr += 2;
    if (wTxbufWrAddr == wTxbufGap)
        wTxbufWrAddr += wTxbufGapdisp << 1;
    wTxbufWrAddr %= 0x2000;

    // Decrement the write counter and trigger an interrupt at the end
    if (wTxbufCount > 0 && --wTxbufCount == 0)
        sendInterrupt(8);
}

void Wifi::writeWTxbufGap(uint16_t mask, uint16_t value)
{
    // Write to the W_TXBUF_GAP register
    mask &= 0x1FFE;
    wTxbufGap = (wTxbufGap & ~mask) | (value & mask);
}

void Wifi::writeWTxbufGapdisp(uint16_t mask, uint16_t value)
{
    // Write to the W_TXBUF_GAPDISP register
    mask &= 0x0FFF;
    wTxbufGapdisp = (wTxbufGapdisp & ~mask) | (value & mask);
}

void Wifi::writeWTxbufLoc(int index, uint16_t mask, uint16_t value)
{
    // Write to one of the W_TXBUF_[BEACON,CMD,LOC1,LOC2,LOC3] registers
    mask &= 0x7FFF;
    wTxbufLoc[index] = (wTxbufLoc[index] & ~mask) | (value & mask);

    // Send a packet to connected cores if triggered
    if (value & BIT(15))
    {
        uint16_t address = (wTxbufLoc[index] & 0xFFF) << 1;
        uint16_t size = core->memory.read<uint16_t>(1, 0x4804000 + address + 0x0A) + 8;
        LOG("Sending packet on channel %d with size 0x%X\n", index, size);

        mutex.lock();

        for (size_t i = 0; i < connections.size(); i++)
        {
            // Read the packet from memory
            uint16_t *data = new uint16_t[size / 2];
            for (size_t j = 0; j < size; j += 2)
                data[j / 2] = core->memory.read<uint16_t>(1, 0x4804000 + address + j);

            // Set the packet size in the outgoing header
            data[4] = size - 12;

            // Add the packet to the queue
            connections[i]->mutex.lock();
            connections[i]->packets.push_back(data);
            connections[i]->mutex.unlock();
        }

        mutex.unlock();

        // Trigger a transmit complete interrupt
        sendInterrupt(1);
    }
}

void Wifi::writeWConfig(int index, uint16_t mask, uint16_t value)
{
    const uint16_t masks[] =
    {
        0x81FF, 0xFFFF, 0xFFFF, 0xFFFF, 0x0FFF,
        0x8FFF, 0xFFFF, 0xFFFF, 0x00FF, 0x00FF,
        0x00FF, 0x00FF, 0xFFFF, 0xFF3F, 0x7A7F
    };

    // Write to one of the W_CONFIG registers
    mask &= masks[index];
    wConfig[index] = (wConfig[index] & ~mask) | (value & mask);
}

void Wifi::writeWBeaconcount2(uint16_t mask, uint16_t value)
{
    // Write to the W_BEACONCOUNT2 register
    wBeaconcount2 = (wBeaconcount2 & ~mask) | (value & mask);
}

void Wifi::writeWBbCnt(uint16_t mask, uint16_t value)
{
    int index = value & 0x00FF;

    // Perform a BB register transfer
    switch ((value & 0xF000) >> 12)
    {
        case 5: // Write
        {
            if ((index >= 0x01 && index <= 0x0C) || (index >= 0x13 && index <= 0x15) || (index >= 0x1B && index <= 0x26) ||
                (index >= 0x28 && index <= 0x4C) || (index >= 0x4E && index <= 0x5C) || (index >= 0x62 && index <= 0x63) ||
                (index == 0x65) || (index >= 0x67 && index <= 0x68)) // Writable registers
                bbRegisters[index] = wBbWrite;
            break;
        }

        case 6: // Read
        {
            wBbRead = bbRegisters[index];
            break;
        }
    }
}

void Wifi::writeWBbWrite(uint16_t mask, uint16_t value)
{
    // Write to the W_BB_WRITE register
    wBbWrite = (wBbWrite & ~mask) | (value & mask);
}

void Wifi::writeWIrfSet(uint16_t mask, uint16_t value)
{
    // Trigger a WiFi interrupt if W_IF & W_IE changes from zero
    if (!(wIe & wIrf) && (wIe & value & mask))
        core->interpreter[1].sendInterrupt(24);

    // Set bits in the W_IF register
    mask &= 0xFBFF;
    wIrf |= (value & mask);
}

uint16_t Wifi::readWRxbufRdData()
{
    // Read a value from WiFi RAM
    uint16_t value = core->memory.read<uint16_t>(1, 0x4804000 + wRxbufRdAddr);

    // Increment the read address
    wRxbufRdAddr += 2;
    if (wRxbufRdAddr == wRxbufGap)
        wRxbufRdAddr += wRxbufGapdisp << 1;
    if ((wRxbufBegin & 0x1FFE) != (wRxbufEnd & 0x1FFE))
        wRxbufRdAddr = (wRxbufBegin & 0x1FFE) + (wRxbufRdAddr - (wRxbufBegin & 0x1FFE)) % ((wRxbufEnd & 0x1FFE) - (wRxbufBegin & 0x1FFE));
    wRxbufRdAddr %= 0x2000;

    // Decrement the read counter and trigger an interrupt at the end
    if (wRxbufCount > 0 && --wRxbufCount == 0)
        sendInterrupt(9);

    return value;
}
