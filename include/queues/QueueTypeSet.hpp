#pragma once

#include "TemplateSet.hpp"

#include "LCRQ.hpp"
#include "LPRQ.hpp"
#include "FAArray.hpp"
#include "LMTQ.hpp"
#include "MuxQueue.hpp"


using UnboundedQueues   = TemplateSet<FAAQueue,LCRQueue,LPRQueue,LinkedMuxQueue,LMTQueue>;
using BoundedQueues     = TemplateSet<BoundedCRQueue,BoundedPRQueue,BoundedMuxQueue,BoundedMTQueue>;
using Queues            = UnboundedQueues::Cat<BoundedQueues>;