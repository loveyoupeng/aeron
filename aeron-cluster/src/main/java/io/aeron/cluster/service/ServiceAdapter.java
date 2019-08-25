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
package io.aeron.cluster.service;

import io.aeron.Subscription;
import io.aeron.cluster.client.ClusterException;
import io.aeron.cluster.codecs.ElectionStartEventDecoder;
import io.aeron.cluster.codecs.JoinLogDecoder;
import io.aeron.cluster.codecs.MessageHeaderDecoder;
import io.aeron.cluster.codecs.ServiceTerminationPositionDecoder;
import io.aeron.logbuffer.FragmentHandler;
import io.aeron.logbuffer.Header;
import org.agrona.CloseHelper;
import org.agrona.DirectBuffer;

final class ServiceAdapter implements FragmentHandler, AutoCloseable
{
    private final Subscription subscription;
    private final ClusteredServiceAgent clusteredServiceAgent;

    private final MessageHeaderDecoder messageHeaderDecoder = new MessageHeaderDecoder();
    private final JoinLogDecoder joinLogDecoder = new JoinLogDecoder();
    private final ServiceTerminationPositionDecoder serviceTerminationPositionDecoder =
        new ServiceTerminationPositionDecoder();
    private final ElectionStartEventDecoder electionStartEventDecoder = new ElectionStartEventDecoder();

    ServiceAdapter(final Subscription subscription, final ClusteredServiceAgent clusteredServiceAgent)
    {
        this.subscription = subscription;
        this.clusteredServiceAgent = clusteredServiceAgent;
    }

    public void close()
    {
        CloseHelper.close(subscription);
    }

    public int poll()
    {
        return subscription.poll(this, 1);
    }

    public void onFragment(final DirectBuffer buffer, final int offset, final int length, final Header header)
    {
        messageHeaderDecoder.wrap(buffer, offset);

        final int schemaId = messageHeaderDecoder.schemaId();
        if (schemaId != MessageHeaderDecoder.SCHEMA_ID)
        {
            throw new ClusterException("expected schemaId=" + MessageHeaderDecoder.SCHEMA_ID + ", actual=" + schemaId);
        }

        final int templateId = messageHeaderDecoder.templateId();
        switch (templateId)
        {
            case JoinLogDecoder.TEMPLATE_ID:
                joinLogDecoder.wrap(
                    buffer,
                    offset + MessageHeaderDecoder.ENCODED_LENGTH,
                    messageHeaderDecoder.blockLength(),
                    messageHeaderDecoder.version());

                clusteredServiceAgent.onJoinLog(
                    joinLogDecoder.leadershipTermId(),
                    joinLogDecoder.logPosition(),
                    joinLogDecoder.maxLogPosition(),
                    joinLogDecoder.memberId(),
                    joinLogDecoder.logSessionId(),
                    joinLogDecoder.logStreamId(),
                    joinLogDecoder.logChannel());
                break;

            case ServiceTerminationPositionDecoder.TEMPLATE_ID:
                serviceTerminationPositionDecoder.wrap(
                    buffer,
                    offset + MessageHeaderDecoder.ENCODED_LENGTH,
                    messageHeaderDecoder.blockLength(),
                    messageHeaderDecoder.version());

                clusteredServiceAgent.onServiceTerminationPosition(serviceTerminationPositionDecoder.logPosition());
                break;

            case ElectionStartEventDecoder.TEMPLATE_ID:
                electionStartEventDecoder.wrap(
                    buffer,
                    offset + MessageHeaderDecoder.ENCODED_LENGTH,
                    messageHeaderDecoder.blockLength(),
                    messageHeaderDecoder.version());

                clusteredServiceAgent.onElectionStartEvent(electionStartEventDecoder.logPosition());
                break;
        }
    }
}
