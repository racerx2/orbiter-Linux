// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#ifndef __CONSOLEINTERPRETER_H
#define __CONSOLEINTERPRETER_H

#include "Interpreter.h"
#include "LuaConsole.h"

// LuaConsole.h includes this header and this header includes LuaConsole.h, so
// whichever is reached first sets its guard and the nested include expands to
// nothing -- leaving the other file's type undeclared. Everything below uses
// LuaConsole only through a pointer, so a forward declaration is enough to
// break the cycle whichever order the two arrive in.
class LuaConsole;

// ==============================================================
// class ConsoleInterpreter

class ConsoleInterpreter: public Interpreter {
	friend class LuaConsole;
public:
	ConsoleInterpreter (LuaConsole *_console);
	void LoadAPI();
	void term_strout (const char *str, bool iserr=false);
	void term_clear ();

protected:
	static int termOut (lua_State *L);
	static int termLineUp (lua_State *L);
	static int termSetVerbosity (lua_State *L);
	static int termClear (lua_State *L);

private:
	LuaConsole *console;
};

#endif // !__CONSOLEINTERPRETER_H