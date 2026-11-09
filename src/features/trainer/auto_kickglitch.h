#pragma once

// What Auto Kickglitch's kick buffer did, for the self-test. Any thread.
namespace auto_kickglitch
{
	struct BufferStats
	{
		int held = 0;           // Kicks pressed in the air and held back
		int onWall = 0;         // ... kicked off the wall once wallrunning
		int late = 0;           // ... let go after the window (no wall came; "kick anyway")
		int dropped = 0;        // ... thrown away (no wall came; "drop it"), or landed / grabbed something first
		bool hooked = false;    // The game's kick press can be held back (its script function is watched)
	};
	BufferStats Stats();
}
