#pragma once

// What the Kickglitch Helper measured last, for the self-test. Any thread.
namespace kickglitch_helper
{
	struct Last
	{
		int attempts = 0;       // Since the module was switched on
		int jumpFrame = -1;     // Frames from landing to the jump of the last attempt; -1: it had no jump
		bool calibrated = false;    // The last attempt was the Kickglitch tool's and set the perfect frame
	};
	Last Get();
}
