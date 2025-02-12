#include "Peer.hpp"

#include <array>
#include <utility>
#include "Room.hpp"

#include "async/message_bus.hpp"
#include "config/configor.hpp"
#include "dto/DTOs.hpp"
#include "oatpp/encoding/Base64.hpp"

#include "middleware/indi_server.hpp"

#include "matchit/matchit.h"

#include "atom/error/exception.hpp"
#include "atom/function/global_ptr.hpp"
#include "atom/log/loguru.hpp"
#include "atom/type/json.hpp"
#include "atom/utils/print.hpp"
#include "atom/utils/string.hpp"

#include "utils/constant.hpp"

using json = nlohmann::json;

void Peer::sendMessageAsync(const oatpp::Object<MessageDto>& message) {
    class SendMessageCoroutine
        : public oatpp::async::Coroutine<SendMessageCoroutine> {
    private:
        oatpp::async::Lock* m_lock_;
        std::shared_ptr<AsyncWebSocket> m_websocket_;
        oatpp::String m_message_;

    public:
        SendMessageCoroutine(oatpp::async::Lock* lock,
                             const std::shared_ptr<AsyncWebSocket>& websocket,
                             oatpp::String message)
            : m_lock_(lock),
              m_websocket_(websocket),
              m_message_(std::move(message)) {}

        auto act() -> Action override {
            return oatpp::async::synchronize(
                       m_lock_, m_websocket_->sendOneFrameTextAsync(m_message_))
                .next(finish());
        }
    };

    if (m_socket_) {
        m_asyncExecutor->execute<SendMessageCoroutine>(
            &m_writeLock_, m_socket_, m_objectMapper->writeToString(message));
    }
}

auto Peer::sendPingAsync() -> bool {
    class SendPingCoroutine
        : public oatpp::async::Coroutine<SendPingCoroutine> {
    private:
        oatpp::async::Lock* m_lock_;
        std::shared_ptr<AsyncWebSocket> m_websocket_;

    public:
        SendPingCoroutine(oatpp::async::Lock* lock,
                          const std::shared_ptr<AsyncWebSocket>& websocket)
            : m_lock_(lock), m_websocket_(websocket) {}

        auto act() -> Action override {
            return oatpp::async::synchronize(
                       m_lock_, m_websocket_->sendPingAsync(nullptr))
                .next(finish());
        }
    };

    /******************************************************
     *
     * Ping counter is increased on sending ping
     * and decreased on receiving pong from the client.
     *
     * If the server didn't receive pong from client
     * before the next ping,- then the client is
     * considered to be disconnected.
     *
     ******************************************************/

    ++m_pingPoingCounter_;

    if (m_socket_ && m_pingPoingCounter_ == 1) {
        m_asyncExecutor->execute<SendPingCoroutine>(&m_writeLock_, m_socket_);
        return true;
    }

    return false;
}

auto Peer::onApiError(const oatpp::String& errorMessage)
    -> oatpp::async::CoroutineStarter {
    class SendErrorCoroutine
        : public oatpp::async::Coroutine<SendErrorCoroutine> {
    private:
        oatpp::async::Lock* m_lock_;
        std::shared_ptr<AsyncWebSocket> m_websocket_;
        oatpp::String m_message_;

    public:
        SendErrorCoroutine(oatpp::async::Lock* lock,
                           const std::shared_ptr<AsyncWebSocket>& websocket,
                           oatpp::String message)
            : m_lock_(lock),
              m_websocket_(websocket),
              m_message_(std::move(message)) {}

        auto act() -> Action override {
            /* synchronized async pipeline */
            return oatpp::async::synchronize(
                       /* Async write-lock to prevent concurrent writes to
                          socket */
                       m_lock_,
                       /* send error message, then close-frame */
                       std::move(m_websocket_->sendOneFrameTextAsync(m_message_)
                                     .next(m_websocket_->sendCloseAsync())))
                .next(
                    /* async error after error message and close-frame are sent
                     */
                    new oatpp::async::Error("API Error"));
        }
    };

    auto message = MessageDto::createShared();
    message->code = MessageCodes::CODE_API_ERROR;
    message->message = errorMessage;

    return SendErrorCoroutine::start(&m_writeLock_, m_socket_,
                                     m_objectMapper->writeToString(message));
}

auto Peer::validateFilesList(const MessageDto::FilesList& filesList)
    -> oatpp::async::CoroutineStarter {
    if (filesList->size() == 0) {
        return onApiError("Files list is empty.");
    }

    for (auto& fileDto : *filesList) {
        if (!fileDto) {
            return onApiError("File structure is not provided.");
        }
        if (!fileDto->clientFileId) {
            return onApiError("File clientId is not provided.");
        }
        if (!fileDto->name) {
            return onApiError("File name is not provided.");
        }
        if (!fileDto->size) {
            return onApiError("File size is not provided.");
        }
    }

    return nullptr;
}

auto Peer::handleFilesMessage(const oatpp::Object<MessageDto>& message)
    -> oatpp::async::CoroutineStarter {
    auto files = message->files;
    validateFilesList(files);

    auto fileMessage = MessageDto::createShared();
    fileMessage->code = MessageCodes::CODE_PEER_MESSAGE_FILE;
    fileMessage->peerId = m_peerId_;
    fileMessage->peerName = m_nickname_;
    fileMessage->timestamp = oatpp::Environment::getMicroTickCount();
    fileMessage->files = MessageDto::FilesList::createShared();

    for (auto& currFile : *files) {
        auto file = m_room_->shareFile(m_peerId_, currFile->clientFileId,
                                       currFile->name, currFile->size);

        auto sharedFile = FileDto::createShared();
        sharedFile->serverFileId = file->getServerFileId();
        sharedFile->name = file->getFileName();
        sharedFile->size = file->getFileSize();

        fileMessage->files->push_back(sharedFile);
    }

    m_room_->addHistoryMessage(fileMessage);
    m_room_->sendMessageAsync(fileMessage);

    return nullptr;
}

auto Peer::handleFileChunkMessage(const oatpp::Object<MessageDto>& message)
    -> oatpp::async::CoroutineStarter {
    auto filesList = message->files;
    if (!filesList) {
        return onApiError("No file provided.");
    }

    if (filesList->size() > 1) {
        return onApiError("Invalid files count. Expected - 1.");
    }

    auto fileDto = filesList->front();
    if (!fileDto) {
        return onApiError("File structure is not provided.");
    }
    if (!fileDto->serverFileId) {
        return onApiError("File clientId is not provided.");
    }
    if (!fileDto->subscriberId) {
        return onApiError("File subscriberId is not provided.");
    }
    if (!fileDto->data) {
        return onApiError("File chunk data is not provided.");
    }

    auto file = m_room_->getFileById(fileDto->serverFileId);

    if (!file) {
        return nullptr;  // Ignore if file doesn't exist. File may be deleted
    }
    // already.

    if (file->getHost()->getPeerId() != getPeerId()) {
        return onApiError("Wrong file host.");
    }

    auto data = oatpp::encoding::Base64::decode(fileDto->data);
    file->provideFileChunk(fileDto->subscriberId, data);

    return nullptr;
}

auto Peer::handleQTextMessage(const std::string& message)
    -> oatpp::async::CoroutineStarter {
    std::vector<std::string> parts = atom::utils::splitString(message, ':');
    // Check if the message is in the correct format
    if (parts.size() != 2 || parts.size() != 3) {
        LOG_F(ERROR, "Invalid message format. {}", message);
        return onApiError("Invalid message format.");
    }
    parts[0] = atom::utils::trim(parts[0]);

    using namespace matchit;
    using namespace lithium::middleware;
    match(parts[0])(
        pattern | "ConfirmIndiDriver" =
            [parts] {
                std::string driverName = atom::utils::trim(parts[1]);
                indiDriverConfirm(driverName);
            },
        pattern | "ConfirmIndiDevice" =
            [parts] {
                std::string deviceName = atom::utils::trim(parts[1]);
                std::string driverName = atom::utils::trim(parts[2]);
                indiDeviceConfirm(deviceName, driverName);
            },
        pattern | "SelectIndiDriver" =
            [parts] {
                std::string driverName = atom::utils::trim(parts[1]);
                int listNum = std::stoi(atom::utils::trim(parts[2]));
                std::shared_ptr<lithium::device::DriversList> driversList;
                GET_OR_CREATE_PTR(driversList, lithium::device::DriversList,
                                  Constants::DRIVERS_LIST)
                printDevGroups2(*driversList, listNum, driverName);
            },
        pattern | "takeExposure" =
            [parts] {
                int expTime = std::stoi(atom::utils::trim(parts[1]));
                LOG_F(INFO, "takeExposure: {}", expTime);
                indiCapture(expTime);
                std::shared_ptr<lithium::ConfigManager> configManager;
                GET_OR_CREATE_PTR(configManager, lithium::ConfigManager,
                                  Constants::CONFIG_MANAGER)
                configManager->setValue(
                    "/lithium/device/camera/current_exposure", expTime);
            },
        pattern | "focusSpeed" =
            [parts] {
                int speed = std::stoi(atom::utils::trim(parts[1]));
                LOG_F(INFO, "focusSpeed: {}", speed);
                int result = setFocusSpeed(speed);
                LOG_F(INFO, "focusSpeed result: {}", result);
                std::shared_ptr<atom::async::MessageBus> messageBusPtr;
                GET_OR_CREATE_PTR(messageBusPtr, atom::async::MessageBus,
                                  Constants::MESSAGE_BUS)
                messageBusPtr->publish(
                    "main", "FocusChangeSpeedSuccess:{}"_fmt(result));
            },
        pattern | "focusMove" =
            [parts] {
                std::string direction = atom::utils::trim(parts[1]);
                int steps = std::stoi(atom::utils::trim(parts[2]));
                LOG_F(INFO, "focusMove: {} {}", direction, steps);
                match(direction)(
                    pattern | "Left" =
                        [steps] {
                            LOG_F(INFO, "focusMove: Left {}", steps);
                            focusMoveAndCalHFR(true, steps);
                        },
                    pattern | "Right" =
                        [steps] {
                            LOG_F(INFO, "focusMove: Right {}", steps);
                            focusMoveAndCalHFR(false, steps);
                        },
                    pattern | "Target" =
                        [steps] {
                            LOG_F(INFO, "focusMove: Up {}", steps);
                            // TODO: Implement FocusGotoAndCalFWHM
                        });
            },
        pattern | "RedBox" =
            [parts] {
                int x = std::stoi(atom::utils::trim(parts[1]));
                int y = std::stoi(atom::utils::trim(parts[2]));
                int w = std::stoi(atom::utils::trim(parts[3]));
                int h = std::stoi(atom::utils::trim(parts[4]));
                LOG_F(INFO, "RedBox: {} {} {} {}", x, y, w, h);
                std::shared_ptr<lithium::ConfigManager> configManager;
                GET_OR_CREATE_PTR(configManager, lithium::ConfigManager,
                                  Constants::CONFIG_MANAGER)
                configManager->setValue("/lithium/device/camera/roi",
                                        std::array<int, 2>({x, y}));
                configManager->setValue("/lithium/device/camera/frame",
                                        std::array<int, 2>({w, h}));
            },
        pattern | "RedBoxSizeChange" =
            [parts] {
                int boxSideLength = std::stoi(atom::utils::trim(parts[1]));
                LOG_F(INFO, "RedBoxSizeChange: {}", boxSideLength);
                std::shared_ptr<lithium::ConfigManager> configManager;
                GET_OR_CREATE_PTR(configManager, lithium::ConfigManager,
                                  Constants::CONFIG_MANAGER)
                configManager->setValue(
                    "/lithium/device/camera/box_side_length", boxSideLength);
                auto [x, y] =
                    configManager->getValue("/lithium/device/camera/frame")
                        ->get<std::array<int, 2>>();
                std::shared_ptr<atom::async::MessageBus> messageBusPtr;
                GET_OR_CREATE_PTR(messageBusPtr, atom::async::MessageBus,
                                  Constants::MESSAGE_BUS)
                messageBusPtr->publish("main",
                                       "MainCameraSize:{}:{}"_fmt(x, y));
            },
        pattern | "AutoFocus" =
            [parts] {
                LOG_F(INFO, "Start AutoFocus");
                autofocus();
            },
        pattern | "StopAutoFocus" =
            [parts] {
                LOG_F(INFO, "Stop AutoFocus");
                std::shared_ptr<lithium::ConfigManager> configManager;
                GET_OR_CREATE_PTR(configManager, lithium::ConfigManager,
                                  Constants::CONFIG_MANAGER)
                configManager->setValue("/lithium/device/focuser/auto_focus",
                                        false);
            },
        pattern | "abortExposure" =
            [parts] {
                LOG_F(INFO, "abortExposure");
                indiAbortCapture();
            },
        pattern | "connectAllDevice" =
            [parts] {
                LOG_F(INFO, "connectAllDevice");
                deviceConnect();
            },
        pattern | "CS" = [parts] { LOG_F(INFO, "CS"); },
        pattern | "disconnectAllDevice" =
            [parts] { LOG_F(INFO, "disconnectAllDevice"); },
        pattern | "MountMoveWest" = [this, parts] {},
        pattern | "MountMoveEast" = [this, parts] {},
        pattern | "MountMoveNorth" = [this, parts] {},
        pattern | "MountMoveSouth" = [this, parts] {},
        pattern | "MountMoveAbort" = [this, parts] {},
        pattern | "MountPark" = [this, parts] {},
        pattern | "MountTrack" = [this, parts] {},
        pattern | "MountHome" = [this, parts] {},
        pattern | "MountSYNC" = [this, parts] {},
        pattern | "MountSpeedSwitch" = [this, parts] {},
        pattern | "ImageGainR" = [this, parts] {},
        pattern | "ImageGainB" = [this, parts] {},
        pattern | "ScheduleTabelData" = [this, parts] {},
        pattern | "MountGoto" = [this, parts] {},
        pattern | "StopSchedule" = [this, parts] {},
        pattern | "CaptureImageSave" = [this, parts] {},
        pattern | "getConnectedDevices" = [this, parts] {},
        pattern | "getStagingImage" = [this, parts] {},
        pattern | "StagingScheduleData" = [this, parts] {},
        pattern | "getStagingGuiderData" = [this, parts] {},
        pattern | "ExpTimeList" = [this, parts] {},
        pattern | "getExpTimeList" = [this, parts] {},
        pattern | "getCaptureStatus" = [this, parts] {},
        pattern | "SetCFWPosition" = [this, parts] {},
        pattern | "CFWList" = [this, parts] {},
        pattern | "getCFWList" = [this, parts] {},
        pattern | "ClearCalibrationData" = [this, parts] {},
        pattern | "GuiderSwitch" = [this, parts] {},
        pattern | "GuiderLoopExpSwitch" = [this, parts] {},
        pattern | "PHD2Recalibrate" = [this, parts] {},
        pattern | "GuiderExpTimeSwitch" = [this, parts] {},
        pattern | "SolveSYNC" = [this, parts] {},
        pattern | "ClearDataPoints" = [this, parts] {},
        pattern | "ShowAllImageFolder" = [this, parts] {},
        pattern | "MoveFileToUSB" = [this, parts] {},
        pattern | "DeleteFile" = [this, parts] {},
        pattern | "USBCheck" =
            [parts] {
                LOG_F(INFO, "USBCheck");
                usbCheck();
            },
        pattern | "SolveImage" = [this, parts] {},
        pattern | "startLoopSolveImage" = [this, parts] {},
        pattern | "stopLoopSolveImage" = [this, parts] {},
        pattern | "StartLoopCapture" = [this, parts] {},
        pattern | "StopLoopCapture" = [this, parts] {},
        pattern | "getStagingSolveResult" = [this, parts] {},
        pattern | "ClearSloveResultList" = [this, parts] {},
        pattern | "getOriginalImage" = [this, parts] {},
        pattern | "saveCurrentLocation" =
            [parts] {
                LOG_F(INFO, "saveCurrentLocation");
                double lat = std::stod(atom::utils::trim(parts[1]));
                double lng = std::stod(atom::utils::trim(parts[2]));
                std::shared_ptr<lithium::ConfigManager> configManager;
                GET_OR_CREATE_PTR(configManager, lithium::ConfigManager,
                                  Constants::CONFIG_MANAGER)
                configManager->setValue("/lithium/location/lat", lat);
                configManager->setValue("/lithium/location/lng", lng);
            },
        pattern | "getCurrentLocation" =
            [parts] {
                LOG_F(INFO, "getCurrentLocation");
                std::shared_ptr<lithium::ConfigManager> configManager;
                GET_OR_CREATE_PTR(configManager, lithium::ConfigManager,
                                  Constants::CONFIG_MANAGER)
                double lat = configManager->getValue("/lithium/location/lat")
                                 ->get<double>();
                double lng = configManager->getValue("/lithium/location/lng")
                                 ->get<double>();
                std::shared_ptr<atom::async::MessageBus> messageBusPtr;
                GET_OR_CREATE_PTR(messageBusPtr, atom::async::MessageBus,
                                  Constants::MESSAGE_BUS)
                messageBusPtr->publish(
                    "main", "SetCurrentLocation:{}:{}"_fmt(lat, lng));
            },
        pattern | "getGPIOsStatus" =
            [parts] {
                LOG_F(INFO, "getGPIOsStatus");
                getGPIOsStatus();
            },
        pattern | "SwitchOutPutPower" =
            [parts] {
                LOG_F(INFO, "SwitchOutPutPower: {}", parts[1]);
                int gpio = std::stoi(atom::utils::trim(parts[1]));
                switchOutPutPower(gpio);
            },
        pattern | "SetBinning" = [this, parts] {},
        pattern | "GuiderCanvasClick" = [this, parts] {},
        pattern | "getQTClientVersion" = [this, parts] {});
}

auto Peer::handleTextMessage(const oatpp::Object<MessageDto>& message)
    -> oatpp::async::CoroutineStarter {
    class SendMessageCoroutine
        : public oatpp::async::Coroutine<SendMessageCoroutine> {
    private:
        oatpp::async::Lock* m_lock_;
        std::shared_ptr<AsyncWebSocket> m_websocket_;
        oatpp::String m_message_;
        json m_response_;
        json m_jsonData_;

    public:
        SendMessageCoroutine(oatpp::async::Lock* lock,
                             const std::shared_ptr<AsyncWebSocket>& websocket,
                             oatpp::String message)
            : m_lock_(lock),
              m_websocket_(websocket),
              m_message_(std::move(message)) {}

        auto act() -> Action override {
            auto command = m_message_.getValue("");
            try {
                m_jsonData_ = json::parse(m_message_.getValue(""));
                yieldTo(&SendMessageCoroutine::process);
            } catch (const json::parse_error& e) {
                m_response_["error"] = "Invalid JSON";
                m_response_["message"] = e.what();
            }
            return yieldTo(&SendMessageCoroutine::send);
        }

        auto process() -> Action {
            if (!m_jsonData_.contains("name")) {
            }
        }

        auto send() -> Action {
            return oatpp::async::synchronize(
                       m_lock_,
                       m_websocket_->sendOneFrameTextAsync(m_response_.dump()))
                .next(finish());
        }
    };
    if (m_socket_) {
        m_asyncExecutor->execute<SendMessageCoroutine>(&m_writeLock_, m_socket_,
                                                       message->message);
    }
}

auto Peer::handleMessage(const oatpp::Object<MessageDto>& message)
    -> oatpp::async::CoroutineStarter {
    if (!message->code) {
        return onApiError("No message code provided.");
    }

    switch (*message->code) {
        case MessageCodes::CODE_PEER_MESSAGE:
            m_room_->addHistoryMessage(message);
            m_room_->sendMessageAsync(message);
            ++m_statistics->EVENT_PEER_SEND_MESSAGE;
            break;

        case MessageCodes::CODE_PEER_COMMAND:
            handleTextMessage(message);
            break;

        case MessageCodes::CODE_PEER_IS_TYPING:
            m_room_->sendMessageAsync(message);
            break;

        case MessageCodes::CODE_FILE_SHARE:
            return handleFilesMessage(message);

        case MessageCodes::CODE_FILE_CHUNK_DATA:
            return handleFileChunkMessage(message);

        default:
            return onApiError("Invalid client message code.");
    }

    return nullptr;
}

auto Peer::getRoom() -> std::shared_ptr<Room> { return m_room_; }

auto Peer::getNickname() -> oatpp::String { return m_nickname_; }

auto Peer::getPeerId() -> v_int64 { return m_peerId_; }

void Peer::addFile(const std::shared_ptr<File>& file) {
    m_files_.push_back(file);
}

auto Peer::getFiles() -> const std::list<std::shared_ptr<File>>& {
    return m_files_;
}

void Peer::invalidateSocket() {
    if (m_socket_) {
        m_socket_->getConnection().invalidate();
    }
    m_socket_.reset();
}

auto Peer::onPing(const std::shared_ptr<AsyncWebSocket>& socket,
                  const oatpp::String& message)
    -> oatpp::async::CoroutineStarter {
    return oatpp::async::synchronize(&m_writeLock_,
                                     socket->sendPongAsync(message));
}

auto Peer::onPong(
    [[maybe_unused]] const std::shared_ptr<AsyncWebSocket>& socket,
    [[maybe_unused]] const oatpp::String& message)
    -> oatpp::async::CoroutineStarter {
    --m_pingPoingCounter_;
    return nullptr;  // do nothing
}

auto Peer::onClose(
    [[maybe_unused]] const std::shared_ptr<AsyncWebSocket>& socket,
    v_uint16 code, [[maybe_unused]] const oatpp::String& message)
    -> oatpp::async::CoroutineStarter {
    return nullptr;  // do nothing
}

auto Peer::readMessage(
    [[maybe_unused]] const std::shared_ptr<AsyncWebSocket>& socket,
    [[maybe_unused]] v_uint8 opcode, p_char8 data,
    oatpp::v_io_size size) -> oatpp::async::CoroutineStarter {
    if (m_messageBuffer_.getCurrentPosition() + size >
        m_appConfig->maxMessageSizeBytes) {
        return onApiError("Message size exceeds max allowed size.");
    }

    if (size == 0) {  // message transfer finished

        auto wholeMessage = m_messageBuffer_.toString();
        m_messageBuffer_.setCurrentPosition(0);

        oatpp::Object<MessageDto> message;

        try {
            message = m_objectMapper->readFromString<oatpp::Object<MessageDto>>(
                wholeMessage);
        } catch (const std::runtime_error& e) {
            return onApiError("Can't parse message");
        }

        message->peerName = m_nickname_;
        message->peerId = m_peerId_;
        message->timestamp = oatpp::Environment::getMicroTickCount();

        return handleMessage(message);
    }
    if (size > 0) {  // message frame received
        m_messageBuffer_.writeSimple(data, size);
    }

    return nullptr;  // do nothing
}
