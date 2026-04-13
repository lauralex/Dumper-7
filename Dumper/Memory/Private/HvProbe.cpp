// NOTE: This translation unit must be compiled with /EHa.
// hv::is_hv_running uses __try/__except(1) to catch the #UD fault that vmcall
// raises when no hypervisor is installed. SEH __except filters in C++ code
// require asynchronous exception handling, not the default /EHsc.

#include "HvProbe.h"
#include "hv.h"

namespace HvProbe
{
	bool IsRunning()
	{
		__try
		{
			return hv::ping() == hv::hypervisor_signature;
		}
		__except (1)
		{
		}
		return false;
	}
}
