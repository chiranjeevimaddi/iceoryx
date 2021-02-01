// Copyright (c) 2019, 2020 by Robert Bosch GmbH, Apex.AI Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "iceoryx_posh/internal/runtime/ipc_base.hpp"
#include "iceoryx_posh/internal/log/posh_logging.hpp"
#include "iceoryx_posh/internal/runtime/message_queue_message.hpp"
#include "iceoryx_utils/cxx/convert.hpp"
#include "iceoryx_utils/cxx/smart_c.hpp"
#include "iceoryx_utils/error_handling/error_handling.hpp"
#include "iceoryx_utils/internal/posix_wrapper/message_queue.hpp"
#include "iceoryx_utils/internal/posix_wrapper/timespec.hpp"

#include <thread>

namespace iox
{
namespace runtime
{
IpcMessageType stringToIpcMessageType(const char* str) noexcept
{
    std::underlying_type<IpcMessageType>::type msg;
    bool noError = cxx::convert::stringIsNumber(str, cxx::convert::NumberType::INTEGER);
    noError &= noError ? (cxx::convert::fromString(str, msg)) : false;
    noError &= noError ? !(static_cast<std::underlying_type<IpcMessageType>::type>(IpcMessageType::BEGIN) >= msg
                           || static_cast<std::underlying_type<IpcMessageType>::type>(IpcMessageType::END) <= msg)
                       : false;
    return noError ? (static_cast<IpcMessageType>(msg)) : IpcMessageType::NOTYPE;
}

std::string IpcMessageTypeToString(const IpcMessageType msg) noexcept
{
    return std::to_string(static_cast<std::underlying_type<IpcMessageType>::type>(msg));
}

IpcMessageErrorType stringToIpcMessageErrorType(const char* str) noexcept
{
    std::underlying_type<IpcMessageErrorType>::type msg;
    bool noError = cxx::convert::stringIsNumber(str, cxx::convert::NumberType::INTEGER);
    noError &= noError ? (cxx::convert::fromString(str, msg)) : false;
    noError &= noError
                   ? !(static_cast<std::underlying_type<IpcMessageErrorType>::type>(IpcMessageErrorType::BEGIN) >= msg
                       || static_cast<std::underlying_type<IpcMessageErrorType>::type>(IpcMessageErrorType::END) <= msg)
                   : false;
    return noError ? (static_cast<IpcMessageErrorType>(msg)) : IpcMessageErrorType::NOTYPE;
}

std::string IpcMessageErrorTypeToString(const IpcMessageErrorType msg) noexcept
{
    return std::to_string(static_cast<std::underlying_type<IpcMessageErrorType>::type>(msg));
}

IpcBase::IpcBase(const ProcessName_t& InterfaceName, const uint64_t maxMessages, const uint64_t messageSize) noexcept
    : m_interfaceName(InterfaceName)
{
    m_maxMessages = maxMessages;
    m_maxMessageSize = messageSize;
    if (m_maxMessageSize > posix::MessageQueue::MAX_MESSAGE_SIZE)
    {
        LogWarn() << "Message size too large, reducing from " << messageSize << " to "
                  << posix::MessageQueue::MAX_MESSAGE_SIZE;
        m_maxMessageSize = posix::MessageQueue::MAX_MESSAGE_SIZE;
    }
}

bool IpcBase::receive(IpcMessage& answer) const noexcept
{
    auto message = m_mq.receive();
    if (message.has_error())
    {
        return false;
    }

    return IpcBase::setMessageFromString(message.value().c_str(), answer);
}

bool IpcBase::timedReceive(const units::Duration timeout, IpcMessage& answer) const noexcept
{
    return !m_mq.timedReceive(timeout)
                .and_then([&answer](auto& message) { IpcBase::setMessageFromString(message.c_str(), answer); })
                .has_error()
           && answer.isValid();
}

bool IpcBase::setMessageFromString(const char* buffer, IpcMessage& answer) noexcept
{
    answer.setMessage(buffer);
    if (!answer.isValid())
    {
        LogError() << "The received message " << answer.getMessage() << " is not valid";
        return false;
    }
    return true;
}

bool IpcBase::send(const IpcMessage& msg) const noexcept
{
    if (!msg.isValid())
    {
        LogError() << "Trying to send the message " << msg.getMessage() << "with mq_send() which "
                   << "does not follow the specified syntax.";
        return false;
    }

    auto logLengthError = [&msg](posix::IpcChannelError& error) {
        if (error == posix::IpcChannelError::MESSAGE_TOO_LONG)
        {
            const size_t messageSize =
                static_cast<size_t>(msg.getMessage().size()) + posix::MessageQueue::NULL_TERMINATOR_SIZE;
            LogError() << "msg size of " << messageSize << "bigger than configured max message size";
        }
    };
    return !m_mq.send(msg.getMessage()).or_else(logLengthError).has_error();
}

bool IpcBase::timedSend(const IpcMessage& msg, units::Duration timeout) const noexcept
{
    if (!msg.isValid())
    {
        LogError() << "Trying to send the message " << msg.getMessage() << " with mq_timedsend() which "
                   << "does not follow the specified syntax.";
        return false;
    }

    auto logLengthError = [&msg](posix::IpcChannelError& error) {
        if (error == posix::IpcChannelError::MESSAGE_TOO_LONG)
        {
            const size_t messageSize =
                static_cast<size_t>(msg.getMessage().size()) + posix::MessageQueue::NULL_TERMINATOR_SIZE;
            LogError() << "msg size of " << messageSize << "bigger than configured max message size";
        }
    };
    return !m_mq.timedSend(msg.getMessage(), timeout).or_else(logLengthError).has_error();
}

const ProcessName_t& IpcBase::getInterfaceName() const noexcept
{
    return m_interfaceName;
}

bool IpcBase::isInitialized() const noexcept
{
    return m_mq.isInitialized();
}

bool IpcBase::openMessageQueue(const posix::IpcChannelSide channelSide) noexcept
{
    m_mq.destroy();

    m_channelSide = channelSide;
    IpcChannelType::create(
        m_interfaceName, posix::IpcChannelMode::BLOCKING, m_channelSide, m_maxMessageSize, m_maxMessages)
        .and_then([this](auto& mq) { this->m_mq = std::move(mq); });

    return m_mq.isInitialized();
}

bool IpcBase::closeMessageQueue() noexcept
{
    return !m_mq.destroy().has_error();
}

bool IpcBase::reopen() noexcept
{
    return openMessageQueue(m_channelSide);
}

bool IpcBase::mqMapsToFile() noexcept
{
    return !m_mq.isOutdated().value_or(true);
}

bool IpcBase::hasClosableMessageQueue() const noexcept
{
    return m_mq.isInitialized();
}

void IpcBase::cleanupOutdatedMessageQueue(const ProcessName_t& name) noexcept
{
    if (posix::MessageQueue::unlinkIfExists(name).value_or(false))
    {
        LogWarn() << "MQ still there, doing an unlink of " << name;
    }
}

} // namespace runtime
} // namespace iox
