#include "game/Foundation.h"
#include "game/Types.h"
#include "game/CCNode.h"
#include "game/CCDirector.h"
#include "game/StateManager.h"
#include "game/GameManager.h"
#include "game/MainManager.h"
#include "game/MenuManager.h"
#include "game/VisualCue.h"
#include "game/Ball.h"
#include "game/UserInfo.h"
#include "game/Table.h"
#include "game/inc/game.h"
#include "game/inc/GameConstants.h"
#include "game/inc/NumberUtils.h"
#include "game/inc/Prediction.fast.h"
// #include "8bp/inc/AutoAim.h"
#include "game/inc/AutoPlay.h"
#include "game/inc/AutoQueue.h"
#include "game/inc/ScreenTable.h"
//#include "game/inc/PocketEffect.h"

// AutoPlay.h above only declares AutoPlay::Update()/ClearState()/etc; the
// actual bodies live in AutoPlay.impl.h. GameMaster.impl.h and HumanScan.impl.h
// are separate files that reuse AutoPlay.impl.h's static helpers
// (CalculateRequiredPower, DistToSegmentSq, EaseInOutCubic), so they must be
// included, in this order, right after it — none of the three were being
// included before, which is why GameMaster/HumanScan/is8BallCushionShotRequired
// showed up as "undeclared identifier" at the call sites in AutoPlay.impl.h.
#include "game/inc/AutoPlay.impl.h"
#include "game/inc/GameMaster.impl.h"
#include "game/inc/HumanScan.impl.h"
