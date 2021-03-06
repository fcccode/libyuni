/*
** This file is part of libyuni, a cross-platform C++ framework (http://libyuni.org).
**
** This Source Code Form is subject to the terms of the Mozilla Public License
** v.2.0. If a copy of the MPL was not distributed with this file, You can
** obtain one at http://mozilla.org/MPL/2.0/.
**
** gitlab: https://gitlab.com/libyuni/libyuni/
** github: https://github.com/libyuni/libyuni/ {mirror}
*/
#pragma once
#include "yuni/yuni.h"
#include "yuni/core/string.h"


namespace Yuni
{

	/*!
	** \brief Escapes a string
	*/
	template<class StringT>
	YUNI_DECL void AppendEscapedString(StringT& out, const AnyString& string, char quote = '"');



} // namespace Yuni

#include "escape.hxx"
