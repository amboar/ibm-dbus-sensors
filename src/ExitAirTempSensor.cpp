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

#include "ExitAirTempSensor.hpp"

#include "Utils.hpp"
#include "VariantVisitors.hpp"

#include <math.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <chrono>
#include <iostream>
#include <limits>
#include <numeric>
#include <sdbusplus/asio/connection.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <vector>

constexpr const float altitudeFactor = 1.14;
constexpr const char* exitAirIface =
    "xyz.openbmc_project.Configuration.ExitAirTempSensor";
constexpr const char* cfmIface = "xyz.openbmc_project.Configuration.CFMSensor";

// todo: this *might* need to be configurable
constexpr const char* inletTemperatureSensor = "temperature/Front_Panel_Temp";
constexpr const char* pidConfigurationType =
    "xyz.openbmc_project.Configuration.Pid";
constexpr const char* settingsDaemon = "xyz.openbmc_project.Settings";
constexpr const char* cfmSettingPath = "/xyz/openbmc_project/control/cfm_limit";
constexpr const char* cfmSettingIface = "xyz.openbmc_project.Control.CFMLimit";

static constexpr bool DEBUG = false;

static constexpr double cfmMaxReading = 255;
static constexpr double cfmMinReading = 0;

static constexpr size_t minSystemCfm = 50;

static void setupSensorMatch(
    std::vector<sdbusplus::bus::match::match>& matches,
    sdbusplus::bus::bus& connection, const std::string& type,
    std::function<void(const double&, sdbusplus::message::message&)>&& callback)
{

    std::function<void(sdbusplus::message::message & message)> eventHandler =
        [callback{std::move(callback)}](sdbusplus::message::message& message) {
            std::string objectName;
            boost::container::flat_map<std::string,
                                       std::variant<double, int64_t>>
                values;
            message.read(objectName, values);
            auto findValue = values.find("Value");
            if (findValue == values.end())
            {
                return;
            }
            double value =
                std::visit(VariantToDoubleVisitor(), findValue->second);
            if (std::isnan(value))
            {
                return;
            }

            callback(value, message);
        };
    matches.emplace_back(connection,
                         "type='signal',"
                         "member='PropertiesChanged',interface='org."
                         "freedesktop.DBus.Properties',path_"
                         "namespace='/xyz/openbmc_project/sensors/" +
                             std::string(type) +
                             "',arg0='xyz.openbmc_project.Sensor.Value'",
                         std::move(eventHandler));
}

static void setMaxPWM(const std::shared_ptr<sdbusplus::asio::connection>& conn,
                      double value)
{
    using GetSubTreeType = std::vector<std::pair<
        std::string,
        std::vector<std::pair<std::string, std::vector<std::string>>>>>;

    conn->async_method_call(
        [conn, value](const boost::system::error_code ec,
                      const GetSubTreeType& ret) {
            if (ec)
            {
                std::cerr << "Error calling mapper\n";
                return;
            }
            for (const auto& [path, objDict] : ret)
            {
                if (objDict.empty())
                {
                    return;
                }
                const std::string& owner = objDict.begin()->first;

                conn->async_method_call(
                    [conn, value, owner,
                     path](const boost::system::error_code ec,
                           const std::variant<std::string>& classType) {
                        if (ec)
                        {
                            std::cerr << "Error getting pid class\n";
                            return;
                        }
                        auto classStr = std::get_if<std::string>(&classType);
                        if (classStr == nullptr || *classStr != "fan")
                        {
                            return;
                        }
                        conn->async_method_call(
                            [](boost::system::error_code& ec) {
                                if (ec)
                                {
                                    std::cerr << "Error setting pid class\n";
                                    return;
                                }
                            },
                            owner, path, "org.freedesktop.DBus.Properties",
                            "Set", pidConfigurationType, "OutLimitMax",
                            std::variant<double>(value));
                    },
                    owner, path, "org.freedesktop.DBus.Properties", "Get",
                    pidConfigurationType, "Class");
            }
        },
        "xyz.openbmc_project.ObjectMapper",
        "/xyz/openbmc_project/object_mapper",
        "xyz.openbmc_project.ObjectMapper", "GetSubTree", "/", 0,
        std::array<std::string, 1>{pidConfigurationType});
}

CFMSensor::CFMSensor(std::shared_ptr<sdbusplus::asio::connection>& conn,
                     const std::string& sensorName,
                     const std::string& sensorConfiguration,
                     sdbusplus::asio::object_server& objectServer,
                     std::vector<thresholds::Threshold>&& thresholds,
                     std::shared_ptr<ExitAirTempSensor>& parent) :
    Sensor(boost::replace_all_copy(sensorName, " ", "_"),
           "" /* todo: remove arg from base*/, std::move(thresholds),
           sensorConfiguration, "xyz.openbmc_project.Configuration.ExitAirTemp",
           cfmMaxReading, cfmMinReading),
    dbusConnection(conn), parent(parent), objServer(objectServer)
{
    sensorInterface =
        objectServer.add_interface("/xyz/openbmc_project/sensors/cfm/" + name,
                                   "xyz.openbmc_project.Sensor.Value");

    if (thresholds::hasWarningInterface(thresholds))
    {
        thresholdInterfaceWarning = objectServer.add_interface(
            "/xyz/openbmc_project/sensors/cfm/" + name,
            "xyz.openbmc_project.Sensor.Threshold.Warning");
    }
    if (thresholds::hasCriticalInterface(thresholds))
    {
        thresholdInterfaceCritical = objectServer.add_interface(
            "/xyz/openbmc_project/sensors/cfm/" + name,
            "xyz.openbmc_project.Sensor.Threshold.Critical");
    }

    association = objectServer.add_interface(
        "/xyz/openbmc_project/sensors/voltage/" + name,
        "org.openbmc.Associations");

    setInitialProperties(conn);
    setupSensorMatch(
        matches, *dbusConnection, "fan_tach",
        std::move(
            [this](const double& value, sdbusplus::message::message& message) {
                tachReadings[message.get_path()] = value;
                if (tachRanges.find(message.get_path()) == tachRanges.end())
                {
                    // calls update reading after updating ranges
                    addTachRanges(message.get_sender(), message.get_path());
                }
                else
                {
                    updateReading();
                }
            }));
    pwmLimitIface =
        objectServer.add_interface("/xyz/openbmc_project/control/pwm_limit",
                                   "xyz.openbmc_project.Control.PWMLimit");
    cfmLimitIface =
        objectServer.add_interface("/xyz/openbmc_project/control/MaxCFM",
                                   "xyz.openbmc_project.Control.CFMLimit");

    conn->async_method_call(
        [this, conn](const boost::system::error_code ec,
                     const std::variant<double> cfmVariant) {
            uint64_t maxRpm = 100;
            if (!ec)
            {

                auto cfm = std::get_if<double>(&cfmVariant);
                if (cfm != nullptr || *cfm >= minSystemCfm)
                {
                    maxRpm = getMaxRpm(*cfm);
                }
            }
            pwmLimitIface->register_property("Limit", maxRpm);
            pwmLimitIface->initialize();
            setMaxPWM(conn, maxRpm);
        },
        settingsDaemon, cfmSettingPath, "org.freedesktop.DBus.Properties",
        "Get", cfmSettingIface, "Limit");

    matches.emplace_back(
        *conn,
        "type='signal',"
        "member='PropertiesChanged',interface='org."
        "freedesktop.DBus.Properties',path='" +
            std::string(cfmSettingPath) + "',arg0='" +
            std::string(cfmSettingIface) + "'",
        [this, conn](sdbusplus::message::message& message) {
            boost::container::flat_map<std::string, std::variant<double>>
                values;
            std::string objectName;
            message.read(objectName, values);
            const auto findValue = values.find("Limit");
            if (findValue == values.end())
            {
                return;
            }
            const auto reading = std::get_if<double>(&(findValue->second));
            if (reading == nullptr)
            {
                std::cerr << "Got CFM Limit of wrong type\n";
                return;
            }
            if (*reading < minSystemCfm && *reading != 0)
            {
                std::cerr << "Illegal CFM setting detected\n";
                return;
            }
            uint64_t maxRpm = getMaxRpm(*reading);
            pwmLimitIface->set_property("Limit", maxRpm);
            setMaxPWM(conn, maxRpm);
        });
}

CFMSensor::~CFMSensor()
{
    objServer.remove_interface(thresholdInterfaceWarning);
    objServer.remove_interface(thresholdInterfaceCritical);
    objServer.remove_interface(sensorInterface);
    objServer.remove_interface(association);
    objServer.remove_interface(cfmLimitIface);
    objServer.remove_interface(pwmLimitIface);
}

void CFMSensor::createMaxCFMIface(void)
{
    cfmLimitIface->register_property("Limit", static_cast<double>(c2) * maxCFM *
                                                  tachs.size());
    cfmLimitIface->initialize();
}

void CFMSensor::addTachRanges(const std::string& serviceName,
                              const std::string& path)
{
    dbusConnection->async_method_call(
        [this, path](const boost::system::error_code ec,
                     const boost::container::flat_map<std::string,
                                                      BasicVariantType>& data) {
            if (ec)
            {
                std::cerr << "Error getting properties from " << path << "\n";
                return;
            }

            double max = loadVariant<double>(data, "MaxValue");
            double min = loadVariant<double>(data, "MinValue");
            tachRanges[path] = std::make_pair(min, max);
            updateReading();
        },
        serviceName, path, "org.freedesktop.DBus.Properties", "GetAll",
        "xyz.openbmc_project.Sensor.Value");
}

void CFMSensor::checkThresholds(void)
{
    thresholds::checkThresholds(this);
}

void CFMSensor::updateReading(void)
{
    double val = 0.0;
    if (calculate(val))
    {
        if (value != val && parent)
        {
            parent->updateReading();
        }
        updateValue(val);
    }
    else
    {
        updateValue(std::numeric_limits<double>::quiet_NaN());
    }
}

uint64_t CFMSensor::getMaxRpm(uint64_t cfmMaxSetting)
{
    uint64_t pwmPercent = 100;
    double totalCFM = std::numeric_limits<double>::max();
    if (cfmMaxSetting == 0)
    {
        return pwmPercent;
    }

    bool firstLoop = true;
    while (totalCFM > cfmMaxSetting)
    {
        if (firstLoop)
        {
            firstLoop = false;
        }
        else
        {
            pwmPercent--;
        }

        double ci = 0;
        if (pwmPercent == 0)
        {
            ci = 0;
        }
        else if (pwmPercent < tachMinPercent)
        {
            ci = c1;
        }
        else if (pwmPercent > tachMaxPercent)
        {
            ci = c2;
        }
        else
        {
            ci = c1 + (((c2 - c1) * (pwmPercent - tachMinPercent)) /
                       (tachMaxPercent - tachMinPercent));
        }

        // Now calculate the CFM for this tach
        // CFMi = Ci * Qmaxi * TACHi
        totalCFM = ci * maxCFM * pwmPercent;
        totalCFM *= tachs.size();
        // divide by 100 since pwm is in percent
        totalCFM /= 100;

        if (pwmPercent <= 0)
        {
            break;
        }
    }

    return pwmPercent;
}

bool CFMSensor::calculate(double& value)
{
    double totalCFM = 0;
    for (const std::string& tachName : tachs)
    {

        auto findReading = std::find_if(
            tachReadings.begin(), tachReadings.end(), [&](const auto& item) {
                return boost::ends_with(item.first, tachName);
            });
        auto findRange = std::find_if(
            tachRanges.begin(), tachRanges.end(), [&](const auto& item) {
                return boost::ends_with(item.first, tachName);
            });
        if (findReading == tachReadings.end())
        {
            if (DEBUG)
            {
                std::cerr << "Can't find " << tachName << "in readings\n";
            }
            continue; // haven't gotten a reading
        }

        if (findRange == tachRanges.end())
        {
            std::cerr << "Can't find " << tachName << " in ranges\n";
            return false; // haven't gotten a max / min
        }

        // avoid divide by 0
        if (findRange->second.second == 0)
        {
            std::cerr << "Tach Max Set to 0 " << tachName << "\n";
            return false;
        }

        double rpm = findReading->second;

        // for now assume the min for a fan is always 0, divide by max to get
        // percent and mult by 100
        rpm /= findRange->second.second;
        rpm *= 100;

        if constexpr (DEBUG)
        {
            std::cout << "Tach " << tachName << "at " << rpm << "\n";
        }

        // Do a linear interpolation to get Ci
        // Ci = C1 + (C2 - C1)/(RPM2 - RPM1) * (TACHi - TACH1)

        double ci = 0;
        if (rpm == 0)
        {
            ci = 0;
        }
        else if (rpm < tachMinPercent)
        {
            ci = c1;
        }
        else if (rpm > tachMaxPercent)
        {
            ci = c2;
        }
        else
        {
            ci = c1 + (((c2 - c1) * (rpm - tachMinPercent)) /
                       (tachMaxPercent - tachMinPercent));
        }

        // Now calculate the CFM for this tach
        // CFMi = Ci * Qmaxi * TACHi
        totalCFM += ci * maxCFM * rpm;
    }

    // divide by 100 since rpm is in percent
    value = totalCFM / 100;
    return true;
}

static constexpr double exitAirMaxReading = 127;
static constexpr double exitAirMinReading = -128;
ExitAirTempSensor::ExitAirTempSensor(
    std::shared_ptr<sdbusplus::asio::connection>& conn,
    const std::string& sensorName, const std::string& sensorConfiguration,
    sdbusplus::asio::object_server& objectServer,
    std::vector<thresholds::Threshold>&& thresholds) :
    Sensor(boost::replace_all_copy(sensorName, " ", "_"),
           "" /* todo: remove arg from base*/, std::move(thresholds),
           sensorConfiguration, "xyz.openbmc_project.Configuration.ExitAirTemp",
           exitAirMaxReading, exitAirMinReading),
    dbusConnection(conn), objServer(objectServer)
{
    sensorInterface = objectServer.add_interface(
        "/xyz/openbmc_project/sensors/temperature/" + name,
        "xyz.openbmc_project.Sensor.Value");

    if (thresholds::hasWarningInterface(thresholds))
    {
        thresholdInterfaceWarning = objectServer.add_interface(
            "/xyz/openbmc_project/sensors/temperature/" + name,
            "xyz.openbmc_project.Sensor.Threshold.Warning");
    }
    if (thresholds::hasCriticalInterface(thresholds))
    {
        thresholdInterfaceCritical = objectServer.add_interface(
            "/xyz/openbmc_project/sensors/temperature/" + name,
            "xyz.openbmc_project.Sensor.Threshold.Critical");
    }
    association = objectServer.add_interface(
        "/xyz/openbmc_project/sensors/temperature/" + name,
        "org.openbmc.Associations");
    setInitialProperties(conn);
    setupMatches();
    setupPowerMatch(conn);
}

ExitAirTempSensor::~ExitAirTempSensor()
{
    objServer.remove_interface(thresholdInterfaceWarning);
    objServer.remove_interface(thresholdInterfaceCritical);
    objServer.remove_interface(sensorInterface);
    objServer.remove_interface(association);
}

void ExitAirTempSensor::setupMatches(void)
{

    constexpr const std::array<const char*, 2> matchTypes = {
        "power", inletTemperatureSensor};

    for (const std::string& type : matchTypes)
    {
        setupSensorMatch(matches, *dbusConnection, type,
                         [this, type](const double& value,
                                      sdbusplus::message::message& message) {
                             if (type == "power")
                             {
                                 powerReadings[message.get_path()] = value;
                             }
                             else if (type == inletTemperatureSensor)
                             {
                                 inletTemp = value;
                             }
                             updateReading();
                         });
    }
    dbusConnection->async_method_call(
        [this](boost::system::error_code ec,
               const std::variant<double>& value) {
            if (ec)
            {
                // sensor not ready yet
                return;
            }

            inletTemp = std::visit(VariantToDoubleVisitor(), value);
        },
        "xyz.openbmc_project.HwmonTempSensor",
        std::string("/xyz/openbmc_project/sensors/") + inletTemperatureSensor,
        "org.freedesktop.DBus.Properties", "Get",
        "xyz.openbmc_project.Sensor.Value", "Value");
}

void ExitAirTempSensor::updateReading(void)
{

    double val = 0.0;
    if (calculate(val))
    {
        val = std::floor(val + 0.5);
        updateValue(val);
    }
    else
    {
        updateValue(std::numeric_limits<double>::quiet_NaN());
    }
}

double ExitAirTempSensor::getTotalCFM(void)
{
    double sum = 0;
    for (auto& sensor : cfmSensors)
    {
        double reading = 0;
        if (!sensor->calculate(reading))
        {
            return -1;
        }
        sum += reading;
    }

    return sum;
}

bool ExitAirTempSensor::calculate(double& val)
{
    static bool firstRead = false;
    double cfm = getTotalCFM();
    if (cfm <= 0)
    {
        std::cerr << "Error getting cfm\n";
        return false;
    }

    // if there is an error getting inlet temp, return error
    if (std::isnan(inletTemp))
    {
        std::cerr << "Cannot get inlet temp\n";
        val = 0;
        return false;
    }

    // if fans are off, just make the exit temp equal to inlet
    if (!isPowerOn())
    {
        val = inletTemp;
        return true;
    }

    double totalPower = 0;
    for (const auto& reading : powerReadings)
    {
        if (std::isnan(reading.second))
        {
            continue;
        }
        totalPower += reading.second;
    }

    // Calculate power correction factor
    // Ci = CL + (CH - CL)/(QMax - QMin) * (CFM - QMin)
    float powerFactor = 0.0;
    if (cfm <= qMin)
    {
        powerFactor = powerFactorMin;
    }
    else if (cfm >= qMax)
    {
        powerFactor = powerFactorMax;
    }
    else
    {
        powerFactor = powerFactorMin + ((powerFactorMax - powerFactorMin) /
                                        (qMax - qMin) * (cfm - qMin));
    }

    totalPower *= powerFactor;
    totalPower += pOffset;

    if (totalPower == 0)
    {
        std::cerr << "total power 0\n";
        val = 0;
        return false;
    }

    if constexpr (DEBUG)
    {
        std::cout << "Power Factor " << powerFactor << "\n";
        std::cout << "Inlet Temp " << inletTemp << "\n";
        std::cout << "Total Power" << totalPower << "\n";
    }

    // Calculate the exit air temp
    // Texit = Tfp + (1.76 * TotalPower / CFM * Faltitude)
    double reading = 1.76 * totalPower * altitudeFactor;
    reading /= cfm;
    reading += inletTemp;

    if constexpr (DEBUG)
    {
        std::cout << "Reading 1: " << reading << "\n";
    }

    // Now perform the exponential average
    // Calculate alpha based on SDR values and CFM
    // Ai = As + (Af - As)/(QMax - QMin) * (CFM - QMin)

    double alpha = 0.0;
    if (cfm < qMin)
    {
        alpha = alphaS;
    }
    else if (cfm >= qMax)
    {
        alpha = alphaF;
    }
    else
    {
        alpha = alphaS + ((alphaF - alphaS) * (cfm - qMin) / (qMax - qMin));
    }

    auto time = std::chrono::system_clock::now();
    if (!firstRead)
    {
        firstRead = true;
        lastTime = time;
        lastReading = reading;
    }
    double alphaDT =
        std::chrono::duration_cast<std::chrono::seconds>(time - lastTime)
            .count() *
        alpha;

    // cap at 1.0 or the below fails
    if (alphaDT > 1.0)
    {
        alphaDT = 1.0;
    }

    if constexpr (DEBUG)
    {
        std::cout << "AlphaDT: " << alphaDT << "\n";
    }

    reading = ((reading * alphaDT) + (lastReading * (1.0 - alphaDT)));

    if constexpr (DEBUG)
    {
        std::cout << "Reading 2: " << reading << "\n";
    }

    val = reading;
    lastReading = reading;
    lastTime = time;
    return true;
}

void ExitAirTempSensor::checkThresholds(void)
{
    thresholds::checkThresholds(this);
}

static void loadVariantPathArray(
    const boost::container::flat_map<std::string, BasicVariantType>& data,
    const std::string& key, std::vector<std::string>& resp)
{
    auto it = data.find(key);
    if (it == data.end())
    {
        std::cerr << "Configuration missing " << key << "\n";
        throw std::invalid_argument("Key Missing");
    }
    BasicVariantType copy = it->second;
    std::vector<std::string> config = std::get<std::vector<std::string>>(copy);
    for (auto& str : config)
    {
        boost::replace_all(str, " ", "_");
    }
    resp = std::move(config);
}

void createSensor(sdbusplus::asio::object_server& objectServer,
                  std::shared_ptr<ExitAirTempSensor>& exitAirSensor,
                  std::shared_ptr<sdbusplus::asio::connection>& dbusConnection)
{
    if (!dbusConnection)
    {
        std::cerr << "Connection not created\n";
        return;
    }
    dbusConnection->async_method_call(
        [&](boost::system::error_code ec, const ManagedObjectType& resp) {
            if (ec)
            {
                std::cerr << "Error contacting entity manager\n";
                return;
            }
            std::vector<std::unique_ptr<CFMSensor>> cfmSensors;
            for (const auto& pathPair : resp)
            {
                for (const auto& entry : pathPair.second)
                {
                    if (entry.first == exitAirIface)
                    {
                        // thresholds should be under the same path
                        std::vector<thresholds::Threshold> sensorThresholds;
                        parseThresholdsFromConfig(pathPair.second,
                                                  sensorThresholds);

                        std::string name =
                            loadVariant<std::string>(entry.second, "Name");
                        exitAirSensor = std::make_shared<ExitAirTempSensor>(
                            dbusConnection, name, pathPair.first.str,
                            objectServer, std::move(sensorThresholds));
                        exitAirSensor->powerFactorMin =
                            loadVariant<double>(entry.second, "PowerFactorMin");
                        exitAirSensor->powerFactorMax =
                            loadVariant<double>(entry.second, "PowerFactorMax");
                        exitAirSensor->qMin =
                            loadVariant<double>(entry.second, "QMin");
                        exitAirSensor->qMax =
                            loadVariant<double>(entry.second, "QMax");
                        exitAirSensor->alphaS =
                            loadVariant<double>(entry.second, "AlphaS");
                        exitAirSensor->alphaF =
                            loadVariant<double>(entry.second, "AlphaF");
                    }
                    else if (entry.first == cfmIface)

                    {
                        // thresholds should be under the same path
                        std::vector<thresholds::Threshold> sensorThresholds;
                        parseThresholdsFromConfig(pathPair.second,
                                                  sensorThresholds);
                        std::string name =
                            loadVariant<std::string>(entry.second, "Name");
                        auto sensor = std::make_unique<CFMSensor>(
                            dbusConnection, name, pathPair.first.str,
                            objectServer, std::move(sensorThresholds),
                            exitAirSensor);
                        loadVariantPathArray(entry.second, "Tachs",
                                             sensor->tachs);
                        sensor->maxCFM =
                            loadVariant<double>(entry.second, "MaxCFM");

                        // change these into percent upon getting the data
                        sensor->c1 =
                            loadVariant<double>(entry.second, "C1") / 100;
                        sensor->c2 =
                            loadVariant<double>(entry.second, "C2") / 100;
                        sensor->tachMinPercent =
                            loadVariant<double>(entry.second,
                                                "TachMinPercent") /
                            100;
                        sensor->tachMaxPercent =
                            loadVariant<double>(entry.second,
                                                "TachMaxPercent") /
                            100;
                        sensor->createMaxCFMIface();

                        cfmSensors.emplace_back(std::move(sensor));
                    }
                }
            }
            if (exitAirSensor)
            {
                exitAirSensor->cfmSensors = std::move(cfmSensors);

                // todo: when power sensors are done delete this fake
                // reading
                exitAirSensor->powerReadings["foo"] = 144.0;

                exitAirSensor->updateReading();
            }
        },
        entityManagerName, "/", "org.freedesktop.DBus.ObjectManager",
        "GetManagedObjects");
}

int main(int argc, char** argv)
{

    boost::asio::io_service io;
    auto systemBus = std::make_shared<sdbusplus::asio::connection>(io);
    systemBus->request_name("xyz.openbmc_project.ExitAirTempSensor");
    sdbusplus::asio::object_server objectServer(systemBus);
    std::shared_ptr<ExitAirTempSensor> sensor =
        nullptr; // wait until we find the config
    std::vector<std::unique_ptr<sdbusplus::bus::match::match>> matches;

    io.post([&]() { createSensor(objectServer, sensor, systemBus); });

    boost::asio::deadline_timer configTimer(io);

    std::function<void(sdbusplus::message::message&)> eventHandler =
        [&](sdbusplus::message::message& message) {
            configTimer.expires_from_now(boost::posix_time::seconds(1));
            // create a timer because normally multiple properties change
            configTimer.async_wait([&](const boost::system::error_code& ec) {
                if (ec == boost::asio::error::operation_aborted)
                {
                    return; // we're being canceled
                }
                createSensor(objectServer, sensor, systemBus);
                if (!sensor)
                {
                    std::cout << "Configuration not detected\n";
                }
            });
        };
    constexpr const std::array<const char*, 2> monitorIfaces = {exitAirIface,
                                                                cfmIface};
    for (const char* type : monitorIfaces)
    {
        auto match = std::make_unique<sdbusplus::bus::match::match>(
            static_cast<sdbusplus::bus::bus&>(*systemBus),
            "type='signal',member='PropertiesChanged',path_namespace='" +
                std::string(inventoryPath) + "',arg0namespace='" + type + "'",
            eventHandler);
        matches.emplace_back(std::move(match));
    }

    io.run();
}