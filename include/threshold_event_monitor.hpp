/*
// Copyright (c) 2018 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#pragma once
#include <sel_logger.hpp>
#include <sensorutils.hpp>

enum class thresholdEventOffsets : uint8_t
{
    lowerNonCritGoingLow = 0x00,
    lowerCritGoingLow = 0x02,
    upperNonCritGoingHigh = 0x07,
    upperCritGoingHigh = 0x09,
};

static constexpr const uint8_t thresholdEventDataTriggerReadingByte2 = (1 << 6);
static constexpr const uint8_t thresholdEventDataTriggerReadingByte3 = (1 << 4);

inline static sdbusplus::bus::match::match startThresholdEventMonitor(
    std::shared_ptr<sdbusplus::asio::connection> conn)
{
    auto thresholdEventMatcherCallback = [conn](
                                             sdbusplus::message::message &msg) {
        // This static set of std::pair<path, event> tracks asserted events to
        // avoid duplicate logs or deasserts logged without an assert
        static boost::container::flat_set<std::pair<std::string, std::string>>
            assertedEvents;
        // SEL event data is three bytes where 0xFF means unspecified
        std::vector<uint8_t> eventData(selEvtDataMaxSize, 0xFF);

        // Get the event type and assertion details from the message
        std::string thresholdInterface;
        boost::container::flat_map<std::string,
                                   sdbusplus::message::variant<bool>>
            propertiesChanged;
        msg.read(thresholdInterface, propertiesChanged);
        std::string event = propertiesChanged.begin()->first;
        bool *pval = sdbusplus::message::variant_ns::get_if<bool>(
            &propertiesChanged.begin()->second);
        if (!pval)
        {
            std::cerr << "threshold event direction has invalid type\n";
            return;
        }
        bool assert = *pval;

        // Check the asserted events to determine if we should log this event
        std::pair<std::string, std::string> pathAndEvent(
            std::string(msg.get_path()), event);
        if (assert)
        {
            // For asserts, add the event to the set and only log it if it's new
            if (assertedEvents.insert(pathAndEvent).second == false)
            {
                // event is already in the set
                return;
            }
        }
        else
        {
            // For deasserts, remove the event and only log the deassert if it
            // was asserted
            if (assertedEvents.erase(pathAndEvent) == 0)
            {
                // asserted event was not in the set
                return;
            }
        }

        // Set the IPMI threshold event type based on the event details from the
        // message
        if (event == "CriticalAlarmLow")
        {
            eventData[0] =
                static_cast<uint8_t>(thresholdEventOffsets::lowerCritGoingLow);
        }
        else if (event == "WarningAlarmLow")
        {
            eventData[0] = static_cast<uint8_t>(
                thresholdEventOffsets::lowerNonCritGoingLow);
        }
        else if (event == "WarningAlarmHigh")
        {
            eventData[0] = static_cast<uint8_t>(
                thresholdEventOffsets::upperNonCritGoingHigh);
        }
        else if (event == "CriticalAlarmHigh")
        {
            eventData[0] =
                static_cast<uint8_t>(thresholdEventOffsets::upperCritGoingHigh);
        }
        // Indicate that bytes 2 and 3 are threshold sensor trigger values
        eventData[0] |= thresholdEventDataTriggerReadingByte2 |
                        thresholdEventDataTriggerReadingByte3;

        // Get the sensor reading to put in the event data
        sdbusplus::message::message getSensorValue =
            conn->new_method_call(msg.get_sender(), msg.get_path(),
                                  "org.freedesktop.DBus.Properties", "GetAll");
        getSensorValue.append("xyz.openbmc_project.Sensor.Value");
        boost::container::flat_map<std::string,
                                   sdbusplus::message::variant<double>>
            sensorValue;
        try
        {
            sdbusplus::message::message getSensorValueResp =
                conn->call(getSensorValue);
            getSensorValueResp.read(sensorValue);
        }
        catch (sdbusplus::exception_t &)
        {
            std::cerr << "error getting sensor value from " << msg.get_path()
                      << "\n";
            return;
        }
        double max = sdbusplus::message::variant_ns::visit(
            ipmi::VariantToDoubleVisitor(), sensorValue["MaxValue"]);
        double min = sdbusplus::message::variant_ns::visit(
            ipmi::VariantToDoubleVisitor(), sensorValue["MinValue"]);
        double sensorVal = sdbusplus::message::variant_ns::visit(
            ipmi::VariantToDoubleVisitor(), sensorValue["Value"]);
        try
        {
            eventData[1] = ipmi::getScaledIPMIValue(sensorVal, max, min);
        }
        catch (std::runtime_error &e)
        {
            std::cerr << e.what();
            eventData[1] = 0xFF;
        }

        // Get the threshold value to put in the event data
        // Get the threshold parameter by removing the "Alarm" text from the
        // event string
        std::string alarm("Alarm");
        if (std::string::size_type pos = event.find(alarm);
            pos != std::string::npos)
        {
            event.erase(pos, alarm.length());
        }
        sdbusplus::message::message getThreshold =
            conn->new_method_call(msg.get_sender(), msg.get_path(),
                                  "org.freedesktop.DBus.Properties", "Get");
        getThreshold.append(thresholdInterface, event);
        sdbusplus::message::variant<double> thresholdValue;
        try
        {
            sdbusplus::message::message getThresholdResp =
                conn->call(getThreshold);
            getThresholdResp.read(thresholdValue);
        }
        catch (sdbusplus::exception_t &)
        {
            std::cerr << "error getting sensor threshold from "
                      << msg.get_path() << "\n";
            return;
        }
        double thresholdVal = sdbusplus::message::variant_ns::visit(
            ipmi::VariantToDoubleVisitor(), thresholdValue);
        try
        {
            eventData[2] = ipmi::getScaledIPMIValue(thresholdVal, max, min);
        }
        catch (std::runtime_error &e)
        {
            std::cerr << e.what();
            eventData[2] = 0xFF;
        }

        // Construct a human-readable message of this event for the log
        std::experimental::string_view sensorName(msg.get_path());
        sensorName.remove_prefix(
            std::min(sensorName.find_last_of("/") + 1, sensorName.size()));
        std::string journalMsg(sensorName.to_string() +
                               (assert ? " asserted " : " deasserted ") +
                               propertiesChanged.begin()->first +
                               ". Reading=" + std::to_string(sensorVal) +
                               " Threshold=" + std::to_string(thresholdVal));

        selAddSystemRecord(journalMsg, std::string(msg.get_path()), eventData,
                           assert, selBMCGenID);
    };
    sdbusplus::bus::match::match thresholdEventMatcher(
        static_cast<sdbusplus::bus::bus &>(*conn),
        "type='signal',interface='org.freedesktop.DBus.Properties',member='"
        "PropertiesChanged',arg0namespace='xyz.openbmc_project.Sensor."
        "Threshold'",
        std::move(thresholdEventMatcherCallback));
    return thresholdEventMatcher;
}