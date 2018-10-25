#pragma once

#include <Thresholds.hpp>
#include <sdbusplus/asio/object_server.hpp>
#include <sensor.hpp>

class ADCSensor : public Sensor
{
  public:
    std::string name;
    std::string configuration;
    ADCSensor(const std::string &path,
              sdbusplus::asio::object_server &objectServer,
              std::shared_ptr<sdbusplus::asio::connection> &conn,
              boost::asio::io_service &io, const std::string &sensorName,
              std::vector<thresholds::Threshold> &&thresholds,
              const double scaleFactor, const std::string &sensorConfiguration);
    ~ADCSensor();

  private:
    std::string path;
    sdbusplus::asio::object_server &objServer;
    boost::asio::posix::stream_descriptor inputDev;
    boost::asio::deadline_timer waitTimer;
    boost::asio::streambuf readBuf;
    int errCount;
    double maxValue;
    double minValue;
    double scaleFactor;
    void setupRead(void);
    void handleResponse(const boost::system::error_code &err);
    void checkThresholds(void);
    void updateValue(const double &newValue);

    void setInitialProperties(
        std::shared_ptr<sdbusplus::asio::connection> &conn);
};
