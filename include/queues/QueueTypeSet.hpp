#pragma once

#include "TemplateSet.hpp"

#include "LinkedAdapter.hpp"
#include "BoundedSegmentAdapter.hpp"
#include "BoundedItemAdapter.hpp"
#include "LCRQ.hpp"
#include "LPRQ.hpp"
#include "FAArray.hpp"
#include "LMTQ.hpp"
#include "MuxQueue.hpp"

using UnboundedQueues   = TemplateSet<  
                                        FAAQueue,
                                        LCRQueue,
                                        LinkedMuxQueue,
                                        LMTQueue,
                                        LPRQueue
                                        >;

using BoundedQueues     = TemplateSet<  
                                        BoundedSegmentCRQueue,
                                        BoundedItemCRQueue,
                                        BoundedMuxQueue,
                                        BoundedSegmentPRQueue,
                                        BoundedItemPRQueue,
                                        BoundedMTQueue
                                        >;
                                        
using Queues            = UnboundedQueues::Cat<BoundedQueues>;