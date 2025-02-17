#pragma once

#include "TemplateSet.hpp"

#include "LCRQ.hpp"
#include "LPRQ.hpp"
#include "FAArray.hpp"
#include "LMTQ.hpp"
#include "MuxQueue.hpp"

using UnboundedQueues   = TemplateSet<  FAAQueue,
                                        LCRQueue,
                                        LinkedMuxQueue,
                                        LMTQueue,
                                        LPRQueue
                                        >;

using BoundedQueues     = TemplateSet<  BoundedSegmentCRQueue,
                                        BoundedItemCRQueue,
                                        BoundedMuxQueue,
                                        BoundedMTQueue,
                                        BoundedSegmentPRQueue,
                                        BoundedItemPRQueue>;
                                        
using Queues            = UnboundedQueues::Cat<BoundedQueues>;