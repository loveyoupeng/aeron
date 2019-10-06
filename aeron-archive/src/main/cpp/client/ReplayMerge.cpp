/*
 * Copyright 2014-2019 Real Logic Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ReplayMerge.h"

using namespace aeron::archive::client;

ReplayMerge::ReplayMerge(
    std::shared_ptr<Subscription> subscription,
    std::shared_ptr<AeronArchive> archive,
    const std::string& replayChannel,
    const std::string& replayDestination,
    const std::string& liveDestination,
    std::int64_t recordingId,
    std::int64_t startPosition) :
    m_subscription(std::move(subscription)),
    m_archive(std::move(archive)),
    m_replayChannel(replayChannel),
    m_replayDestination(replayDestination),
    m_liveDestination(liveDestination),
    m_recordingId(recordingId),
    m_startPosition(startPosition),
    m_liveAddThreshold(REPLAY_MERGE_LIVE_ADD_THRESHOLD)
{
    std::shared_ptr<ChannelUri> subscriptionChannelUri = ChannelUri::parse(m_subscription->channel());

    if (subscriptionChannelUri->get(MDC_CONTROL_MODE_PARAM_NAME) != MDC_CONTROL_MODE_MANUAL)
    {
        throw util::IllegalArgumentException("subscription channel must be manual control mode: mode=" +
            subscriptionChannelUri->get(MDC_CONTROL_MODE_PARAM_NAME), SOURCEINFO);
    }

    m_subscription->addDestination(m_replayDestination);
}

ReplayMerge::~ReplayMerge()
{
    if (State::CLOSED != m_state)
    {
        if (!m_archive->context().aeron()->isClosed())
        {
            if (State::MERGED != m_state && State::STOP_REPLAY != m_state)
            {
                m_subscription->removeDestination(m_replayDestination);
            }

            if (m_isReplayActive)
            {
                m_isReplayActive = false;
                const std::int64_t correlationId = m_archive->context().aeron()->nextCorrelationId();
                m_archive->archiveProxy().stopReplay(m_replaySessionId, correlationId, m_archive->controlSessionId());
            }
        }

        state(State::CLOSED);
    }
}

int ReplayMerge::getRecordingPosition()
{
    int workCount = 0;

    if (aeron::NULL_VALUE == m_activeCorrelationId)
    {
        const std::int64_t correlationId = m_archive->context().aeron()->nextCorrelationId();

        if (m_archive->archiveProxy().getRecordingPosition(m_recordingId, correlationId, m_archive->controlSessionId()))
        {
            m_activeCorrelationId = correlationId;
            workCount += 1;
        }
    }
    else if (pollForResponse(*m_archive, m_activeCorrelationId))
    {
        m_nextTargetPosition = m_archive->controlResponsePoller().relevantId();
        m_activeCorrelationId = aeron::NULL_VALUE;
        if (NULL_POSITION == m_nextTargetPosition)
        {
            const std::int64_t correlationId = m_archive->context().aeron()->nextCorrelationId();

            if (m_archive->archiveProxy().getStopPosition(m_recordingId, correlationId, m_archive->controlSessionId()))
            {
                m_activeCorrelationId = correlationId;
                workCount += 1;
            }
        }
        else
        {
            m_initialMaxPosition = m_nextTargetPosition;
            state(State::REPLAY);
        }

        workCount += 1;
    }

    return workCount;
}

int ReplayMerge::replay()
{
    int workCount = 0;

    if (aeron::NULL_VALUE == m_activeCorrelationId)
    {
        const std::int64_t correlationId = m_archive->context().aeron()->nextCorrelationId();

        if (m_archive->archiveProxy().replay(
            m_recordingId,
            m_startPosition,
            std::numeric_limits<std::int64_t>::max(),
            m_replayChannel,
            m_subscription->streamId(),
            correlationId,
            m_archive->controlSessionId()))
        {
            m_activeCorrelationId = correlationId;
            workCount += 1;
        }
    }
    else if (pollForResponse(*m_archive, m_activeCorrelationId))
    {
        m_isReplayActive = true;
        m_replaySessionId = m_archive->controlResponsePoller().relevantId();
        m_activeCorrelationId = aeron::NULL_VALUE;
        state(State::CATCHUP);
        workCount += 1;
    }

    return workCount;
}

int ReplayMerge::catchup()
{
    int workCount = 0;

    if (nullptr == m_image && m_subscription->isConnected())
    {
        m_image = m_subscription->imageBySessionId(static_cast<std::int32_t>(m_replaySessionId));
    }

    if (nullptr != m_image && m_image->position() >= m_nextTargetPosition)
    {
        m_activeCorrelationId = aeron::NULL_VALUE;
        state(State::ATTEMPT_LIVE_JOIN);
        workCount += 1;
    }

    return workCount;
}

int ReplayMerge::attemptLiveJoin()
{
    int workCount = 0;

    if (aeron::NULL_VALUE == m_activeCorrelationId)
    {
        const std::int64_t correlationId = m_archive->context().aeron()->nextCorrelationId();

        if (m_archive->archiveProxy().getRecordingPosition(m_recordingId, correlationId, m_archive->controlSessionId()))
        {
            m_activeCorrelationId = correlationId;
            workCount += 1;
        }
    }
    else if (pollForResponse(*m_archive, m_activeCorrelationId))
    {
        m_nextTargetPosition = m_archive->controlResponsePoller().relevantId();
        m_activeCorrelationId = aeron::NULL_VALUE;
        if (NULL_POSITION == m_nextTargetPosition)
        {
            const std::int64_t correlationId = m_archive->context().aeron()->nextCorrelationId();

            if (m_archive->archiveProxy().getRecordingPosition(
                m_recordingId, correlationId, m_archive->controlSessionId()))
            {
                m_activeCorrelationId = correlationId;
            }
        }
        else
        {
            State nextState = State::CATCHUP;

            if (nullptr != m_image)
            {
                const std::int64_t position = m_image->position();

                if (shouldAddLiveDestination(position))
                {
                    m_subscription->addDestination(m_liveDestination);
                    m_isLiveAdded = true;
                }
                else if (shouldStopAndRemoveReplay(position))
                {
                    m_subscription->removeDestination(m_replayDestination);
                    nextState = State::STOP_REPLAY;
                }
            }

            state(nextState);
        }

        workCount += 1;
    }

    return workCount;
}

int ReplayMerge::stopReplay()
{
    int workCount = 0;
    const std::int64_t correlationId = m_archive->context().aeron()->nextCorrelationId();

    if (m_archive->archiveProxy().stopReplay(m_replaySessionId, correlationId, m_archive->controlSessionId()))
    {
        m_isReplayActive = false;
        state(State::MERGED);
        workCount += 1;
    }

    return workCount;
}

bool ReplayMerge::pollForResponse(AeronArchive& archive, std::int64_t correlationId)
{
    ControlResponsePoller& poller = archive.controlResponsePoller();

    if (poller.poll() > 0 && poller.isPollComplete())
    {
        if (poller.controlSessionId() == archive.controlSessionId() && poller.correlationId() == correlationId)
        {
            if (poller.isCodeError())
            {
                throw ArchiveException(static_cast<std::int32_t>(poller.relevantId()),
                    "archive response for correlationId=" + std::to_string(correlationId) +
                    ", error: " + poller.errorMessage(), SOURCEINFO);
            }

            return true;
        }
    }

    return false;
}
