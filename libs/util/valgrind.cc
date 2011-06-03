// Copyright 2009 The RE2 Authors.  All Rights Reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

#include "util/util.h"
#include "util/valgrind.h"

namespace re2 {

static bool checkValgrind() {
#ifdef RUNNING_ON_VALGRIND
#if defined(_MSC_VER)
	RUNNING_ON_VALGRIND;
	return _qzz_res != 0;
#else
	return RUNNING_ON_VALGRIND;
#endif
#else
	return false;
#endif
}

static const int valgrind = checkValgrind();

int RunningOnValgrind() {
  return valgrind;
}

}  // namespace re2
