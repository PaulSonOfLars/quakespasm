/*
Copyright (C) 1996-2001 Id Software, Inc.
Copyright (C) 2002-2005 John Fitzgibbons and others
Copyright (C) 2007-2008 Kristian Duske
Copyright (C) 2010-2014 QuakeSpasm developers

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "quakedef.h"
#if defined(SDL_FRAMEWORK) || defined(NO_SDL_CONFIG)
#if defined(USE_SDL2)
#include <SDL2/SDL.h>
#else
#include <SDL/SDL.h>
#endif
#else
#include "SDL.h"
#endif

static qboolean	textmode;

static cvar_t in_debugkeys = {"in_debugkeys", "0", CVAR_NONE};

#ifdef __APPLE__
/* Mouse acceleration needs to be disabled on OS X */
#define MACOS_X_ACCELERATION_HACK
#endif

#ifdef MACOS_X_ACCELERATION_HACK
#include <IOKit/IOTypes.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDParameter.h>
#include <IOKit/hidsystem/event_status_driver.h>
#endif

// SDL2 Game Controller cvars
cvar_t	joy_deadzone = { "joy_deadzone", "0.2", CVAR_NONE };
cvar_t	joy_deadzone_trigger = { "joy_deadzone_trigger", "0.001", CVAR_NONE };
cvar_t	joy_sensitivity_yaw = { "joy_sensitivity_yaw", "300", CVAR_NONE };
cvar_t	joy_sensitivity_pitch = { "joy_sensitivity_pitch", "150", CVAR_NONE };
cvar_t	joy_invert = { "joy_invert", "0", CVAR_NONE };
cvar_t	joy_exponent = { "joy_exponent", "3", CVAR_NONE };
cvar_t	joy_swapmovelook = { "joy_swapmovelook", "0", CVAR_NONE };
cvar_t	joy_enable = { "joy_enable", "1", CVAR_NONE };

#if defined(USE_SDL2)
static SDL_JoystickID joy_active_instaceid = -1;
static SDL_GameController *joy_active_controller = NULL;
#endif

static qboolean	no_mouse = false;

static int buttonremap[] =
{
	K_MOUSE1,
	K_MOUSE3,	/* right button		*/
	K_MOUSE2,	/* middle button	*/
#if !defined(USE_SDL2)	/* mousewheel up/down not counted as buttons in SDL2 */
	K_MWHEELUP,
	K_MWHEELDOWN,
#endif
	K_MOUSE4,
	K_MOUSE5
};

/* total accumulated mouse movement since last frame */
static int	total_dx, total_dy = 0;

static int IN_FilterMouseEvents (const SDL_Event *event)
{
	switch (event->type)
	{
	case SDL_MOUSEMOTION:
	// case SDL_MOUSEBUTTONDOWN:
	// case SDL_MOUSEBUTTONUP:
		return 0;
	}

	return 1;
}

#if defined(USE_SDL2)
static int IN_SDL2_FilterMouseEvents (void *userdata, SDL_Event *event)
{
	return IN_FilterMouseEvents (event);
}
#endif

static void IN_BeginIgnoringMouseEvents()
{
#if defined(USE_SDL2)
	SDL_EventFilter currentFilter = NULL;
	void *currentUserdata = NULL;
	SDL_GetEventFilter(&currentFilter, &currentUserdata);

	if (currentFilter != IN_SDL2_FilterMouseEvents)
		SDL_SetEventFilter(IN_SDL2_FilterMouseEvents, NULL);
#else
	if (SDL_GetEventFilter() != IN_FilterMouseEvents)
		SDL_SetEventFilter(IN_FilterMouseEvents);
#endif
}

static void IN_EndIgnoringMouseEvents()
{
#if defined(USE_SDL2)
	SDL_EventFilter currentFilter;
	void *currentUserdata;
	if (SDL_GetEventFilter(&currentFilter, &currentUserdata) == SDL_TRUE)
		SDL_SetEventFilter(NULL, NULL);
#else
	if (SDL_GetEventFilter() != NULL)
		SDL_SetEventFilter(NULL);
#endif
}

#ifdef MACOS_X_ACCELERATION_HACK
static cvar_t in_disablemacosxmouseaccel = {"in_disablemacosxmouseaccel", "1", CVAR_ARCHIVE};
static double originalMouseSpeed = -1.0;

static io_connect_t IN_GetIOHandle(void)
{
	io_connect_t iohandle = MACH_PORT_NULL;
	io_service_t iohidsystem = MACH_PORT_NULL;
	mach_port_t masterport;
	kern_return_t status;

	status = IOMasterPort(MACH_PORT_NULL, &masterport);
	if (status != KERN_SUCCESS)
		return 0;

	iohidsystem = IORegistryEntryFromPath(masterport, kIOServicePlane ":/IOResources/IOHIDSystem");
	if (!iohidsystem)
		return 0;

	status = IOServiceOpen(iohidsystem, mach_task_self(), kIOHIDParamConnectType, &iohandle);
	IOObjectRelease(iohidsystem);

	return iohandle;
}

static void IN_DisableOSXMouseAccel (void)
{
	io_connect_t mouseDev = IN_GetIOHandle();
	if (mouseDev != 0)
	{
		if (IOHIDGetAccelerationWithKey(mouseDev, CFSTR(kIOHIDMouseAccelerationType), &originalMouseSpeed) == kIOReturnSuccess)
		{
			if (IOHIDSetAccelerationWithKey(mouseDev, CFSTR(kIOHIDMouseAccelerationType), -1.0) != kIOReturnSuccess)
			{
				Cvar_Set("in_disablemacosxmouseaccel", "0");
				Con_Printf("WARNING: Could not disable mouse acceleration (failed at IOHIDSetAccelerationWithKey).\n");
			}
		}
		else
		{
			Cvar_Set("in_disablemacosxmouseaccel", "0");
			Con_Printf("WARNING: Could not disable mouse acceleration (failed at IOHIDGetAccelerationWithKey).\n");
		}
		IOServiceClose(mouseDev);
	}
	else
	{
		Cvar_Set("in_disablemacosxmouseaccel", "0");
		Con_Printf("WARNING: Could not disable mouse acceleration (failed at IO_GetIOHandle).\n");
	}
}

static void IN_ReenableOSXMouseAccel (void)
{
	io_connect_t mouseDev = IN_GetIOHandle();
	if (mouseDev != 0)
	{
		if (IOHIDSetAccelerationWithKey(mouseDev, CFSTR(kIOHIDMouseAccelerationType), originalMouseSpeed) != kIOReturnSuccess)
			Con_Printf("WARNING: Could not re-enable mouse acceleration (failed at IOHIDSetAccelerationWithKey).\n");
		IOServiceClose(mouseDev);
	}
	else
	{
		Con_Printf("WARNING: Could not re-enable mouse acceleration (failed at IO_GetIOHandle).\n");
	}
	originalMouseSpeed = -1;
}
#endif /* MACOS_X_ACCELERATION_HACK */


void IN_Activate (void)
{
	if (no_mouse)
		return;

#ifdef MACOS_X_ACCELERATION_HACK
	/* Save the status of mouse acceleration */
	if (originalMouseSpeed == -1 && in_disablemacosxmouseaccel.value)
		IN_DisableOSXMouseAccel();
#endif

#if defined(USE_SDL2)
	if (SDL_SetRelativeMouseMode(SDL_TRUE) != 0)
	{
		Con_Printf("WARNING: SDL_SetRelativeMouseMode(SDL_TRUE) failed.\n");
	}
#else
	if (SDL_WM_GrabInput(SDL_GRAB_QUERY) != SDL_GRAB_ON)
	{
		SDL_WM_GrabInput(SDL_GRAB_ON);
		if (SDL_WM_GrabInput(SDL_GRAB_QUERY) != SDL_GRAB_ON)
			Con_Printf("WARNING: SDL_WM_GrabInput(SDL_GRAB_ON) failed.\n");
	}

	if (SDL_ShowCursor(SDL_QUERY) != SDL_DISABLE)
	{
		SDL_ShowCursor(SDL_DISABLE);
		if (SDL_ShowCursor(SDL_QUERY) != SDL_DISABLE)
			Con_Printf("WARNING: SDL_ShowCursor(SDL_DISABLE) failed.\n");
	}
#endif

	IN_EndIgnoringMouseEvents();

	total_dx = 0;
	total_dy = 0;
}

void IN_Deactivate (qboolean free_cursor)
{
	if (no_mouse)
		return;

#ifdef MACOS_X_ACCELERATION_HACK
	if (originalMouseSpeed != -1)
		IN_ReenableOSXMouseAccel();
#endif

	if (free_cursor)
	{
#if defined(USE_SDL2)
		SDL_SetRelativeMouseMode(SDL_FALSE);
#else
		if (SDL_WM_GrabInput(SDL_GRAB_QUERY) != SDL_GRAB_OFF)
		{
			SDL_WM_GrabInput(SDL_GRAB_OFF);
			if (SDL_WM_GrabInput(SDL_GRAB_QUERY) != SDL_GRAB_OFF)
				Con_Printf("WARNING: SDL_WM_GrabInput(SDL_GRAB_OFF) failed.\n");
		}

		if (SDL_ShowCursor(SDL_QUERY) != SDL_ENABLE)
		{
			SDL_ShowCursor(SDL_ENABLE);
			if (SDL_ShowCursor(SDL_QUERY) != SDL_ENABLE)
				Con_Printf("WARNING: SDL_ShowCursor(SDL_ENABLE) failed.\n");
		}
#endif
	}

	/* discard all mouse events when input is deactivated */
	IN_BeginIgnoringMouseEvents();
}

void IN_StartupJoystick (void)
{
#if defined(USE_SDL2)
	int i;
	int nummappings;
	char controllerdb[MAX_OSPATH];
	SDL_GameController *gamecontroller;
	
	if (COM_CheckParm("-nojoy"))
		return;
	
	// Load additional SDL2 controller definitions from gamecontrollerdb.txt
	q_snprintf (controllerdb, sizeof(controllerdb), "%s/gamecontrollerdb.txt", com_basedir);
	nummappings = SDL_GameControllerAddMappingsFromFile(controllerdb);
	if (nummappings > 0)
		Con_Printf("%d mappings loaded from gamecontrollerdb.txt\n", nummappings);
	
	// Also try host_parms->userdir
	if (host_parms->userdir != host_parms->basedir)
	{
		q_snprintf (controllerdb, sizeof(controllerdb), "%s/gamecontrollerdb.txt", host_parms->userdir);
		nummappings = SDL_GameControllerAddMappingsFromFile(controllerdb);
		if (nummappings > 0)
			Con_Printf("%d mappings loaded from gamecontrollerdb.txt\n", nummappings);
	}
	
	if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) == -1 )
	{
		Con_Printf("WARNING: Could not initialize SDL Game Controller\n");
		return;
	}

	for (i = 0; i < SDL_NumJoysticks(); i++)
	{
		if ( SDL_IsGameController(i) )
		{
			gamecontroller = SDL_GameControllerOpen(i);
			if (gamecontroller)
			{
				Con_Printf("detected controller: %s\n", SDL_GameControllerNameForIndex(i));
				
				joy_active_instaceid = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gamecontroller));
				joy_active_controller = gamecontroller;
				break;
			}
		}
	}
#endif
}

void IN_ShutdownJoystick (void)
{
#if defined(USE_SDL2)
	SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
#endif
}

void IN_Init (void)
{
	textmode = Key_TextEntry();

#if !defined(USE_SDL2)
	SDL_EnableUNICODE (textmode);
	if (SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY, SDL_DEFAULT_REPEAT_INTERVAL) == -1)
		Con_Printf("Warning: SDL_EnableKeyRepeat() failed.\n");
#else
	if (textmode)
		SDL_StartTextInput();
	else
		SDL_StopTextInput();
#endif
	if (safemode || COM_CheckParm("-nomouse"))
	{
		no_mouse = true;
		/* discard all mouse events when input is deactivated */
		IN_BeginIgnoringMouseEvents();
	}

#ifdef MACOS_X_ACCELERATION_HACK
	Cvar_RegisterVariable(&in_disablemacosxmouseaccel);
#endif
	Cvar_RegisterVariable(&in_debugkeys);
	Cvar_RegisterVariable(&joy_sensitivity_yaw);
	Cvar_RegisterVariable(&joy_sensitivity_pitch);
	Cvar_RegisterVariable(&joy_deadzone);
	Cvar_RegisterVariable(&joy_deadzone_trigger);
	Cvar_RegisterVariable(&joy_invert);
	Cvar_RegisterVariable(&joy_exponent);
	Cvar_RegisterVariable(&joy_swapmovelook);
	Cvar_RegisterVariable(&joy_enable);

	IN_Activate();
	IN_StartupJoystick();
}

void IN_Shutdown (void)
{
	IN_Deactivate(true);
	IN_ShutdownJoystick();
}

extern cvar_t cl_maxpitch; /* johnfitz -- variable pitch clamping */
extern cvar_t cl_minpitch; /* johnfitz -- variable pitch clamping */


void IN_MouseMotion(int dx, int dy)
{
	total_dx += dx;
	total_dy += dy;
}

#if defined(USE_SDL2)
typedef struct joyaxis_s
{
	float x;
	float y;
} joyaxis_t;

typedef struct joy_buttonstate_s
{
	qboolean buttondown[SDL_CONTROLLER_BUTTON_MAX];
} joybuttonstate_t;

typedef struct axisstate_s
{
	float axisvalue[SDL_CONTROLLER_AXIS_MAX]; // normalized to +-1
} joyaxisstate_t;

static joybuttonstate_t joy_buttonstate;
static joyaxisstate_t joy_axisstate;

static double joy_buttontimer[SDL_CONTROLLER_BUTTON_MAX];
static double joy_emulatedkeytimer[10];

/*
================
IN_ApplyEasing

assumes axis values are in [-1, 1]. Raises the axis values to the given exponent, keeping signs.
================
*/
static joyaxis_t IN_ApplyEasing(joyaxis_t axis, float exponent)
{
	joyaxis_t result = {0};
	float magnitude, eased_magnitude;
	
	magnitude = sqrtf( (axis.x * axis.x) + (axis.y * axis.y) );

	if (magnitude > 1)
		magnitude = 1;
	
	if (magnitude == 0)
		return result;
	
	eased_magnitude = powf(magnitude, exponent);
	
	result.x = axis.x * (eased_magnitude / magnitude);
	result.y = axis.y * (eased_magnitude / magnitude);
	return result;
}

/*
================
IN_ApplyMoveEasing

clamps coordinates to a square with coordinates +/- sqrt(2)/2, then scales them to +/- 1.
This wastes a bit of stick range, but gives the diagonals coordinates of (+/-1,+/-1),
so holding the stick on a diagonal gives the same speed boost as holding the forward and strafe keyboard keys.
================
*/
static joyaxis_t IN_ApplyMoveEasing(joyaxis_t axis)
{
	joyaxis_t result = {0};
	const float v = sqrtf(2.0f) / 2.0f;
	
	result.x = q_max(-v, q_min(v, axis.x));
	result.y = q_max(-v, q_min(v, axis.y));
	
	result.x /= v;
	result.y /= v;

	return result;
}


/*
================
IN_ApplyDeadzone

from https://github.com/jeremiah-sypult/Quakespasm-Rift
and adapted from http://www.third-helix.com/2013/04/12/doing-thumbstick-dead-zones-right.html
================
*/
static joyaxis_t IN_ApplyDeadzone(joyaxis_t axis, float deadzone)
{
	joyaxis_t result = {0};
	float magnitude = sqrtf( (axis.x * axis.x) + (axis.y * axis.y) );
	
	if ( magnitude < deadzone ) {
		result.x = result.y = 0.0f;
	} else {
		joyaxis_t normalized;
		float gradient;
		
		if ( magnitude > 1.0f ) {
			magnitude = 1.0f;
		}
		
		normalized.x = axis.x / magnitude;
		normalized.y = axis.y / magnitude;
		gradient = ( (magnitude - deadzone) / (1.0f - deadzone) );
		result.x = normalized.x * gradient;
		result.y = normalized.y * gradient;
	}
	
	return result;
}

/*
================
IN_KeyForControllerButton
================
*/
static int IN_KeyForControllerButton(SDL_GameControllerButton button)
{
	switch (button)
	{
		case SDL_CONTROLLER_BUTTON_A: return K_ABUTTON;
		case SDL_CONTROLLER_BUTTON_B: return K_BBUTTON;
		case SDL_CONTROLLER_BUTTON_X: return K_XBUTTON;
		case SDL_CONTROLLER_BUTTON_Y: return K_YBUTTON;
		case SDL_CONTROLLER_BUTTON_BACK: return K_TAB;
		case SDL_CONTROLLER_BUTTON_START: return K_ESCAPE;
		case SDL_CONTROLLER_BUTTON_LEFTSTICK: return K_LTHUMB;
		case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return K_RTHUMB;
		case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return K_LSHOULDER;
		case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return K_RSHOULDER;
		case SDL_CONTROLLER_BUTTON_DPAD_UP: return K_UPARROW;
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return K_DOWNARROW;
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return K_LEFTARROW;
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return K_RIGHTARROW;
		default: return 0;
	}
}

/*
================
IN_JoyKeyEvent

Sends a Key_Event if a unpressed -> pressed or pressed -> unpressed transition occurred,
and generates key repeats if the button is held down.

Adapted from DarkPlaces by lordhavoc
================
*/
static void IN_JoyKeyEvent(qboolean wasdown, qboolean isdown, int key, double *timer)
{
	// we can't use `realtime` for key repeats because it is not monotomic
	const double currenttime = Sys_DoubleTime();
	
	if (wasdown)
	{
		if (isdown)
		{
			if (currenttime >= *timer)
			{
				*timer = currenttime + 0.1;
				Key_Event(key, true);
			}
		}
		else
		{
			*timer = 0;
			Key_Event(key, false);
		}
	}
	else
	{
		if (isdown)
		{
			*timer = currenttime + 0.5;
			Key_Event(key, true);
		}
	}
}
#endif

/*
================
IN_Commands

Emit key events for game controller buttons, including emulated buttons for analog sticks/triggers
================
*/
void IN_Commands (void)
{
#if defined(USE_SDL2)
	joyaxisstate_t newaxisstate;
	int i;
	const float stickthreshold = 0.9;
	const float triggerthreshold = joy_deadzone_trigger.value;
	
	if (!joy_enable.value)
		return;
	
	if (!joy_active_controller)
		return;

	// emit key events for controller buttons
	for (i = 0; i < SDL_CONTROLLER_BUTTON_MAX; i++)
	{
		qboolean newstate = SDL_GameControllerGetButton(joy_active_controller, i);
		qboolean oldstate = joy_buttonstate.buttondown[i];
		
		joy_buttonstate.buttondown[i] = newstate;
		
		// NOTE: This can cause a reentrant call of IN_Commands, via SCR_ModalMessage when confirming a new game.
		IN_JoyKeyEvent(oldstate, newstate, IN_KeyForControllerButton(i), &joy_buttontimer[i]);
	}
	
	for (i = 0; i < SDL_CONTROLLER_AXIS_MAX; i++)
	{
		newaxisstate.axisvalue[i] = SDL_GameControllerGetAxis(joy_active_controller, i) / 32768.0f;
	}
	
	// emit emulated arrow keys so the analog sticks can be used in the menu
	if (key_dest != key_game)
	{
		IN_JoyKeyEvent(joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_LEFTX] < -stickthreshold, newaxisstate.axisvalue[SDL_CONTROLLER_AXIS_LEFTX] < -stickthreshold, K_LEFTARROW, &joy_emulatedkeytimer[0]);
		IN_JoyKeyEvent(joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_LEFTX] > stickthreshold,  newaxisstate.axisvalue[SDL_CONTROLLER_AXIS_LEFTX] > stickthreshold, K_RIGHTARROW, &joy_emulatedkeytimer[1]);
		IN_JoyKeyEvent(joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_LEFTY] < -stickthreshold, newaxisstate.axisvalue[SDL_CONTROLLER_AXIS_LEFTY] < -stickthreshold, K_UPARROW, &joy_emulatedkeytimer[2]);
		IN_JoyKeyEvent(joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_LEFTY] > stickthreshold,  newaxisstate.axisvalue[SDL_CONTROLLER_AXIS_LEFTY] > stickthreshold, K_DOWNARROW, &joy_emulatedkeytimer[3]);
		IN_JoyKeyEvent(joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_RIGHTX] < -stickthreshold,newaxisstate.axisvalue[SDL_CONTROLLER_AXIS_RIGHTX] < -stickthreshold, K_LEFTARROW, &joy_emulatedkeytimer[4]);
		IN_JoyKeyEvent(joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_RIGHTX] > stickthreshold, newaxisstate.axisvalue[SDL_CONTROLLER_AXIS_RIGHTX] > stickthreshold, K_RIGHTARROW, &joy_emulatedkeytimer[5]);
		IN_JoyKeyEvent(joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_RIGHTY] < -stickthreshold,newaxisstate.axisvalue[SDL_CONTROLLER_AXIS_RIGHTY] < -stickthreshold, K_UPARROW, &joy_emulatedkeytimer[6]);
		IN_JoyKeyEvent(joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_RIGHTY] > stickthreshold, newaxisstate.axisvalue[SDL_CONTROLLER_AXIS_RIGHTY] > stickthreshold, K_DOWNARROW, &joy_emulatedkeytimer[7]);
	}
	
	// emit emulated keys for the analog triggers
	IN_JoyKeyEvent(joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_TRIGGERLEFT] > triggerthreshold,  newaxisstate.axisvalue[SDL_CONTROLLER_AXIS_TRIGGERLEFT] > triggerthreshold, K_LTRIGGER, &joy_emulatedkeytimer[8]);
	IN_JoyKeyEvent(joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] > triggerthreshold, newaxisstate.axisvalue[SDL_CONTROLLER_AXIS_TRIGGERRIGHT] > triggerthreshold, K_RTRIGGER, &joy_emulatedkeytimer[9]);
	
	joy_axisstate = newaxisstate;
#endif
}

/*
================
IN_JoyMove
================
*/
void IN_JoyMove (usercmd_t *cmd)
{
#if defined(USE_SDL2)
	float	speed;
	joyaxis_t moveAxis, lookAxis;

	if (!joy_enable.value)
		return;
	
	if (!joy_active_controller)
		return;
	
	moveAxis.x = joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_LEFTX];
	moveAxis.y = joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_LEFTY];
	lookAxis.x = joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_RIGHTX];
	lookAxis.y = joy_axisstate.axisvalue[SDL_CONTROLLER_AXIS_RIGHTY];
	
	if (joy_swapmovelook.value)
	{
		joyaxis_t temp = moveAxis;
		moveAxis = lookAxis;
		lookAxis = temp;
	}
	
	moveAxis = IN_ApplyDeadzone(moveAxis, joy_deadzone.value);
	lookAxis = IN_ApplyDeadzone(lookAxis, joy_deadzone.value);

	moveAxis = IN_ApplyMoveEasing(moveAxis);
	lookAxis = IN_ApplyEasing(lookAxis, joy_exponent.value);
	
	if (in_speed.state & 1)
		speed = cl_movespeedkey.value;
	else
		speed = 1;

	cmd->sidemove += (cl_sidespeed.value * speed * moveAxis.x);
	cmd->forwardmove -= (cl_forwardspeed.value * speed * moveAxis.y);

	cl.viewangles[YAW] -= lookAxis.x * joy_sensitivity_yaw.value * host_frametime;
	cl.viewangles[PITCH] += lookAxis.y * joy_sensitivity_pitch.value * (joy_invert.value ? -1.0 : 1.0) * host_frametime;

	if (lookAxis.x != 0 || lookAxis.y != 0)
		V_StopPitchDrift();

	/* johnfitz -- variable pitch clamping */
	if (cl.viewangles[PITCH] > cl_maxpitch.value)
		cl.viewangles[PITCH] = cl_maxpitch.value;
	if (cl.viewangles[PITCH] < cl_minpitch.value)
		cl.viewangles[PITCH] = cl_minpitch.value;
#endif
}

void IN_MouseMove(usercmd_t *cmd)
{
	int		dmx, dmy;

	dmx = total_dx * sensitivity.value;
	dmy = total_dy * sensitivity.value;

	total_dx = 0;
	total_dy = 0;

	if ( (in_strafe.state & 1) || (lookstrafe.value && (in_mlook.state & 1) ))
		cmd->sidemove += m_side.value * dmx;
	else
		cl.viewangles[YAW] -= m_yaw.value * dmx;

	if (in_mlook.state & 1)
	{
		if (dmx || dmy)
			V_StopPitchDrift ();
	}

	if ( (in_mlook.state & 1) && !(in_strafe.state & 1))
	{
		cl.viewangles[PITCH] += m_pitch.value * dmy;
		/* johnfitz -- variable pitch clamping */
		if (cl.viewangles[PITCH] > cl_maxpitch.value)
			cl.viewangles[PITCH] = cl_maxpitch.value;
		if (cl.viewangles[PITCH] < cl_minpitch.value)
			cl.viewangles[PITCH] = cl_minpitch.value;
	}
	else
	{
		if ((in_strafe.state & 1) && noclip_anglehack)
			cmd->upmove -= m_forward.value * dmy;
		else
			cmd->forwardmove -= m_forward.value * dmy;
	}
}

void IN_Move(usercmd_t *cmd)
{
	IN_JoyMove(cmd);
	IN_MouseMove(cmd);
}

void IN_ClearStates (void)
{
}

void IN_UpdateInputMode (void)
{
	qboolean want_textmode = Key_TextEntry();
	if (textmode != want_textmode)
	{
		textmode = want_textmode;
#if !defined(USE_SDL2)
		SDL_EnableUNICODE(textmode);
#else
		if (textmode)
			SDL_StartTextInput();
		else
			SDL_StopTextInput();
#endif
	}
}

#if !defined(USE_SDL2)
static inline int IN_SDL_KeysymToQuakeKey(SDLKey sym)
{
	if (sym > SDLK_SPACE && sym < SDLK_DELETE)
		return sym;

	switch (sym)
	{
	case SDLK_TAB: return K_TAB;
	case SDLK_RETURN: return K_ENTER;
	case SDLK_ESCAPE: return K_ESCAPE;
	case SDLK_SPACE: return K_SPACE;

	case SDLK_BACKSPACE: return K_BACKSPACE;
	case SDLK_UP: return K_UPARROW;
	case SDLK_DOWN: return K_DOWNARROW;
	case SDLK_LEFT: return K_LEFTARROW;
	case SDLK_RIGHT: return K_RIGHTARROW;

	case SDLK_LALT: return K_ALT;
	case SDLK_RALT: return K_ALT;
	case SDLK_LCTRL: return K_CTRL;
	case SDLK_RCTRL: return K_CTRL;
	case SDLK_LSHIFT: return K_SHIFT;
	case SDLK_RSHIFT: return K_SHIFT;

	case SDLK_F1: return K_F1;
	case SDLK_F2: return K_F2;
	case SDLK_F3: return K_F3;
	case SDLK_F4: return K_F4;
	case SDLK_F5: return K_F5;
	case SDLK_F6: return K_F6;
	case SDLK_F7: return K_F7;
	case SDLK_F8: return K_F8;
	case SDLK_F9: return K_F9;
	case SDLK_F10: return K_F10;
	case SDLK_F11: return K_F11;
	case SDLK_F12: return K_F12;
	case SDLK_INSERT: return K_INS;
	case SDLK_DELETE: return K_DEL;
	case SDLK_PAGEDOWN: return K_PGDN;
	case SDLK_PAGEUP: return K_PGUP;
	case SDLK_HOME: return K_HOME;
	case SDLK_END: return K_END;

	case SDLK_NUMLOCK: return K_KP_NUMLOCK;
	case SDLK_KP_DIVIDE: return K_KP_SLASH;
	case SDLK_KP_MULTIPLY: return K_KP_STAR;
	case SDLK_KP_MINUS:return K_KP_MINUS;
	case SDLK_KP7: return K_KP_HOME;
	case SDLK_KP8: return K_KP_UPARROW;
	case SDLK_KP9: return K_KP_PGUP;
	case SDLK_KP_PLUS: return K_KP_PLUS;
	case SDLK_KP4: return K_KP_LEFTARROW;
	case SDLK_KP5: return K_KP_5;
	case SDLK_KP6: return K_KP_RIGHTARROW;
	case SDLK_KP1: return K_KP_END;
	case SDLK_KP2: return K_KP_DOWNARROW;
	case SDLK_KP3: return K_KP_PGDN;
	case SDLK_KP_ENTER: return K_KP_ENTER;
	case SDLK_KP0: return K_KP_INS;
	case SDLK_KP_PERIOD: return K_KP_DEL;

	case SDLK_LMETA: return K_COMMAND;
	case SDLK_RMETA: return K_COMMAND;

	case SDLK_BREAK: return K_PAUSE;
	case SDLK_PAUSE: return K_PAUSE;

	case SDLK_WORLD_18: return '~'; // the '�' key

	default: return 0;
	}
}
#endif

#if defined(USE_SDL2)
static inline int IN_SDL2_ScancodeToQuakeKey(SDL_Scancode scancode)
{
	switch (scancode)
	{
	case SDL_SCANCODE_TAB: return K_TAB;
	case SDL_SCANCODE_RETURN: return K_ENTER;
	case SDL_SCANCODE_RETURN2: return K_ENTER;
	case SDL_SCANCODE_ESCAPE: return K_ESCAPE;
	case SDL_SCANCODE_SPACE: return K_SPACE;

	case SDL_SCANCODE_A: return 'a';
	case SDL_SCANCODE_B: return 'b';
	case SDL_SCANCODE_C: return 'c';
	case SDL_SCANCODE_D: return 'd';
	case SDL_SCANCODE_E: return 'e';
	case SDL_SCANCODE_F: return 'f';
	case SDL_SCANCODE_G: return 'g';
	case SDL_SCANCODE_H: return 'h';
	case SDL_SCANCODE_I: return 'i';
	case SDL_SCANCODE_J: return 'j';
	case SDL_SCANCODE_K: return 'k';
	case SDL_SCANCODE_L: return 'l';
	case SDL_SCANCODE_M: return 'm';
	case SDL_SCANCODE_N: return 'n';
	case SDL_SCANCODE_O: return 'o';
	case SDL_SCANCODE_P: return 'p';
	case SDL_SCANCODE_Q: return 'q';
	case SDL_SCANCODE_R: return 'r';
	case SDL_SCANCODE_S: return 's';
	case SDL_SCANCODE_T: return 't';
	case SDL_SCANCODE_U: return 'u';
	case SDL_SCANCODE_V: return 'v';
	case SDL_SCANCODE_W: return 'w';
	case SDL_SCANCODE_X: return 'x';
	case SDL_SCANCODE_Y: return 'y';
	case SDL_SCANCODE_Z: return 'z';

	case SDL_SCANCODE_1: return '1';
	case SDL_SCANCODE_2: return '2';
	case SDL_SCANCODE_3: return '3';
	case SDL_SCANCODE_4: return '4';
	case SDL_SCANCODE_5: return '5';
	case SDL_SCANCODE_6: return '6';
	case SDL_SCANCODE_7: return '7';
	case SDL_SCANCODE_8: return '8';
	case SDL_SCANCODE_9: return '9';
	case SDL_SCANCODE_0: return '0';

	case SDL_SCANCODE_MINUS: return '-';
	case SDL_SCANCODE_EQUALS: return '=';
	case SDL_SCANCODE_LEFTBRACKET: return '[';
	case SDL_SCANCODE_RIGHTBRACKET: return ']';
	case SDL_SCANCODE_BACKSLASH: return '\\';
	case SDL_SCANCODE_NONUSHASH: return '#';
	case SDL_SCANCODE_SEMICOLON: return ';';
	case SDL_SCANCODE_APOSTROPHE: return '\'';
	case SDL_SCANCODE_GRAVE: return '`';
	case SDL_SCANCODE_COMMA: return ',';
	case SDL_SCANCODE_PERIOD: return '.';
	case SDL_SCANCODE_SLASH: return '/';
	case SDL_SCANCODE_NONUSBACKSLASH: return '\\';

	case SDL_SCANCODE_BACKSPACE: return K_BACKSPACE;
	case SDL_SCANCODE_UP: return K_UPARROW;
	case SDL_SCANCODE_DOWN: return K_DOWNARROW;
	case SDL_SCANCODE_LEFT: return K_LEFTARROW;
	case SDL_SCANCODE_RIGHT: return K_RIGHTARROW;

	case SDL_SCANCODE_LALT: return K_ALT;
	case SDL_SCANCODE_RALT: return K_ALT;
	case SDL_SCANCODE_LCTRL: return K_CTRL;
	case SDL_SCANCODE_RCTRL: return K_CTRL;
	case SDL_SCANCODE_LSHIFT: return K_SHIFT;
	case SDL_SCANCODE_RSHIFT: return K_SHIFT;

	case SDL_SCANCODE_F1: return K_F1;
	case SDL_SCANCODE_F2: return K_F2;
	case SDL_SCANCODE_F3: return K_F3;
	case SDL_SCANCODE_F4: return K_F4;
	case SDL_SCANCODE_F5: return K_F5;
	case SDL_SCANCODE_F6: return K_F6;
	case SDL_SCANCODE_F7: return K_F7;
	case SDL_SCANCODE_F8: return K_F8;
	case SDL_SCANCODE_F9: return K_F9;
	case SDL_SCANCODE_F10: return K_F10;
	case SDL_SCANCODE_F11: return K_F11;
	case SDL_SCANCODE_F12: return K_F12;
	case SDL_SCANCODE_INSERT: return K_INS;
	case SDL_SCANCODE_DELETE: return K_DEL;
	case SDL_SCANCODE_PAGEDOWN: return K_PGDN;
	case SDL_SCANCODE_PAGEUP: return K_PGUP;
	case SDL_SCANCODE_HOME: return K_HOME;
	case SDL_SCANCODE_END: return K_END;

	case SDL_SCANCODE_NUMLOCKCLEAR: return K_KP_NUMLOCK;
	case SDL_SCANCODE_KP_DIVIDE: return K_KP_SLASH;
	case SDL_SCANCODE_KP_MULTIPLY: return K_KP_STAR;
	case SDL_SCANCODE_KP_MINUS: return K_KP_MINUS;
	case SDL_SCANCODE_KP_7: return K_KP_HOME;
	case SDL_SCANCODE_KP_8: return K_KP_UPARROW;
	case SDL_SCANCODE_KP_9: return K_KP_PGUP;
	case SDL_SCANCODE_KP_PLUS: return K_KP_PLUS;
	case SDL_SCANCODE_KP_4: return K_KP_LEFTARROW;
	case SDL_SCANCODE_KP_5: return K_KP_5;
	case SDL_SCANCODE_KP_6: return K_KP_RIGHTARROW;
	case SDL_SCANCODE_KP_1: return K_KP_END;
	case SDL_SCANCODE_KP_2: return K_KP_DOWNARROW;
	case SDL_SCANCODE_KP_3: return K_KP_PGDN;
	case SDL_SCANCODE_KP_ENTER: return K_KP_ENTER;
	case SDL_SCANCODE_KP_0: return K_KP_INS;
	case SDL_SCANCODE_KP_PERIOD: return K_KP_DEL;

	case SDL_SCANCODE_LGUI: return K_COMMAND;
	case SDL_SCANCODE_RGUI: return K_COMMAND;

	case SDL_SCANCODE_PAUSE: return K_PAUSE;

	default: return 0;
	}
}
#endif

#if defined(USE_SDL2)
static void IN_DebugTextEvent(SDL_Event *event)
{
	Con_Printf ("SDL_TEXTINPUT '%s'\n", event->text.text);
}
#endif

static void IN_DebugKeyEvent(SDL_Event *event)
{
	const char *eventtype = (event->key.state == SDL_PRESSED) ? "SDL_KEYDOWN" : "SDL_KEYUP";
#if defined(USE_SDL2)
	Con_Printf ("%s scancode: '%s' keycode: '%s'\n",
		eventtype,
		SDL_GetScancodeName(event->key.keysym.scancode),
		SDL_GetKeyName(event->key.keysym.sym));
#else
	Con_Printf ("%s sym: '%s' unicode: %04x\n",
		eventtype,
		SDL_GetKeyName(event->key.keysym.sym),
		(int)event->key.keysym.unicode);
#endif
}

void IN_SendKeyEvents (void)
{
	SDL_Event event;
	int key;
	qboolean down;

	while (SDL_PollEvent(&event))
	{
		switch (event.type)
		{
#if defined(USE_SDL2)
		case SDL_WINDOWEVENT:
			if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED)
				S_UnblockSound();
			else if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
				S_BlockSound();
			break;
#else
		case SDL_ACTIVEEVENT:
			if (event.active.state & (SDL_APPINPUTFOCUS|SDL_APPACTIVE))
			{
				if (event.active.gain)
					S_UnblockSound();
				else	S_BlockSound();
			}
			break;
#endif
#if defined(USE_SDL2)
		case SDL_TEXTINPUT:
			if (in_debugkeys.value)
				IN_DebugTextEvent(&event);

		// SDL2: We use SDL_TEXTINPUT for typing in the console / chat.
		// SDL2 uses the local keyboard layout and handles modifiers
		// (shift for uppercase, etc.) for us.
			{
				unsigned char *ch;
				for (ch = (unsigned char *)event.text.text; *ch; ch++)
					if ((*ch & ~0x7F) == 0)
						Char_Event (*ch);
			}
			break;
#endif
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			down = (event.key.state == SDL_PRESSED);

			if (in_debugkeys.value)
				IN_DebugKeyEvent(&event);

#if defined(USE_SDL2)
		// SDL2: we interpret the keyboard as the US layout, so keybindings
		// are based on key position, not the label on the key cap.
			key = IN_SDL2_ScancodeToQuakeKey(event.key.keysym.scancode);
#else
			key = IN_SDL_KeysymToQuakeKey(event.key.keysym.sym);
#endif

			Key_Event (key, down);

#if !defined(USE_SDL2)
			if (down && (event.key.keysym.unicode & ~0x7F) == 0)
				Char_Event (event.key.keysym.unicode);
#endif
			break;

		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			if (event.button.button < 1 ||
			    event.button.button > sizeof(buttonremap) / sizeof(buttonremap[0]))
			{
				Con_Printf ("Ignored event for mouse button %d\n",
							event.button.button);
				break;
			}
			Key_Event(buttonremap[event.button.button - 1], event.button.state == SDL_PRESSED);
			break;

#if defined(USE_SDL2)
		case SDL_MOUSEWHEEL:
			if (event.wheel.y > 0)
			{
				Key_Event(K_MWHEELUP, true);
				Key_Event(K_MWHEELUP, false);
			}
			else if (event.wheel.y < 0)
			{
				Key_Event(K_MWHEELDOWN, true);
				Key_Event(K_MWHEELDOWN, false);
			}
			break;
#endif

		case SDL_MOUSEMOTION:
			IN_MouseMotion(event.motion.xrel, event.motion.yrel);
			break;

#if defined(USE_SDL2)
		case SDL_CONTROLLERDEVICEADDED:
			if (joy_active_instaceid == -1)
			{
				joy_active_controller = SDL_GameControllerOpen(event.cdevice.which);
				if (joy_active_controller == NULL)
					Con_DPrintf("Couldn't open game controller\n");
				else
				{
					SDL_Joystick *joy;
					joy = SDL_GameControllerGetJoystick(joy_active_controller);
					joy_active_instaceid = SDL_JoystickInstanceID(joy);
				}
			}
			else
				Con_DPrintf("Ignoring SDL_CONTROLLERDEVICEADDED\n");
			break;
		case SDL_CONTROLLERDEVICEREMOVED:
			if (joy_active_instaceid != -1 && event.cdevice.which == joy_active_instaceid)
			{
				SDL_GameControllerClose(joy_active_controller);
				joy_active_controller = NULL;
				joy_active_instaceid = -1;
			}
			else
				Con_DPrintf("Ignoring SDL_CONTROLLERDEVICEREMOVED\n");
			break;
		case SDL_CONTROLLERDEVICEREMAPPED:
			Con_DPrintf("Ignoring SDL_CONTROLLERDEVICEREMAPPED\n");
			break;
#endif
				
		case SDL_QUIT:
			CL_Disconnect ();
			Sys_Quit ();
			break;

		default:
			break;
		}
	}
}

