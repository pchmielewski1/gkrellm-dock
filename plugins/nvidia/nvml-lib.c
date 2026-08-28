/*****************************************************************************
 * GKrellM nVidia — NVML dynamic loader
 *****************************************************************************/
#include "nvml-lib.h"
#include <dlfcn.h>
#include <string.h>

#ifndef	FALSE
 #define FALSE (0)
#endif
#ifndef	TRUE
 #define TRUE (!FALSE)
#endif
#ifndef	NULL
 #define NULL (0)
#endif

#ifndef GKFREQ_NVML_SONAME
 #define GKFREQ_NVML_SONAME "libnvidia-ml.so.1"
#endif

static const char *k_nvml_candidates[] = {
	"libnvidia-ml.so.1",
	"libnvidia-ml.so",
	GKFREQ_NVML_SONAME,
	NULL
};

void shutdown_gpulib(GKNVMLLib *lib)
{
	if (is_valid_gpulib(lib) && lib->nvmlShutdown) {
		lib->nvmlShutdown();
		dlclose(lib->handle);

		lib->handle = NULL;
		lib->valid = FALSE;
	}
}

boolean is_valid_gpulib_path(char *path)
{
	boolean res = FALSE;
	void *tmp_handle = NULL;
	nvmlInit_fn tmp_fn = NULL;

	tmp_handle = (path && path[0] != '\0') ? dlopen(path, RTLD_LAZY) : NULL;
	dlerror();
	tmp_fn = (tmp_handle) ? (nvmlInit_fn)dlsym(tmp_handle, "nvmlInit") : NULL;
	res = (tmp_fn != NULL);
	if (tmp_handle)
		dlclose(tmp_handle);
	return res;
}

boolean is_valid_gpulib(GKNVMLLib *lib)
{
	return lib && lib->handle && lib->valid;
}

static boolean bind_required(void *handle, const char *name, void **out)
{
	const char *err;
	dlerror();
	*out = dlsym(handle, name);
	err = dlerror();
	return (*out != NULL && err == NULL);
}

boolean initialize_gpulib(GKNVMLLib *lib)
{
	boolean res = FALSE;
	const char **cand;
	char try_path[512];

	if (!lib)
		return FALSE;

	if (lib->path[0] == '\0')
		strncpy(lib->path, GKFREQ_NVML_SONAME, sizeof(lib->path) - 1);

	/* Try configured path first, then common SONAMEs. */
	try_path[0] = '\0';
	if (is_valid_gpulib_path(lib->path))
		strncpy(try_path, lib->path, sizeof(try_path) - 1);
	else {
		for (cand = k_nvml_candidates; *cand; ++cand) {
			if (is_valid_gpulib_path((char *)*cand)) {
				strncpy(try_path, *cand, sizeof(try_path) - 1);
				strncpy(lib->path, *cand, sizeof(lib->path) - 1);
				break;
			}
		}
	}

	if (try_path[0] == '\0') {
		lib->valid = FALSE;
		return FALSE;
	}

	lib->handle = dlopen(try_path, RTLD_LAZY);
	if (!lib->handle) {
		lib->valid = FALSE;
		return FALSE;
	}

	res = TRUE;
	res &= bind_required(lib->handle, "nvmlInit", (void **)&lib->nvmlInit);
	res &= bind_required(lib->handle, "nvmlShutdown", (void **)&lib->nvmlShutdown);
	res &= bind_required(lib->handle, "nvmlDeviceGetCount",
	                     (void **)&lib->nvmlDeviceGetCount);
	res &= bind_required(lib->handle, "nvmlDeviceGetHandleByIndex",
	                     (void **)&lib->nvmlDeviceGetHandleByIndex);
	res &= bind_required(lib->handle, "nvmlDeviceGetName",
	                     (void **)&lib->nvmlDeviceGetName);
	res &= bind_required(lib->handle, "nvmlDeviceGetClockInfo",
	                     (void **)&lib->nvmlDeviceGetClockInfo);
	res &= bind_required(lib->handle, "nvmlDeviceGetTemperature",
	                     (void **)&lib->nvmlDeviceGetTemperature);
	res &= bind_required(lib->handle, "nvmlDeviceGetPowerUsage",
	                     (void **)&lib->nvmlDeviceGetPowerUsage);
	res &= bind_required(lib->handle, "nvmlDeviceGetUtilizationRates",
	                     (void **)&lib->nvmlDeviceGetUtilizationRates);
	res &= bind_required(lib->handle, "nvmlDeviceGetPciInfo",
	                     (void **)&lib->nvmlDeviceGetPciInfo);

	/* Optional — GB10 / some drivers omit fan or memory-v2 symbols */
	dlerror();
	lib->nvmlDeviceGetFanSpeed_v2 =
		(nvmlDeviceGetFanSpeed_v2_fn)dlsym(lib->handle, "nvmlDeviceGetFanSpeed_v2");
	lib->nvmlDeviceGetMemoryInfo_v2 =
		(nvmlDeviceGetMemoryInfo_v2_fn)dlsym(lib->handle, "nvmlDeviceGetMemoryInfo_v2");
	lib->nvmlDeviceGetNumFans =
		(nvmlDeviceGetNumFans_fn)dlsym(lib->handle, "nvmlDeviceGetNumFans");
	lib->nvmlDeviceGetFanSpeedRPM =
		(nvmlDeviceGetFanSpeedRPM_fn)dlsym(lib->handle, "nvmlDeviceGetFanSpeedRPM");

	if (res)
		res = (lib->nvmlInit() == NVML_SUCCESS);

	lib->valid = res;
	if (!res && lib->handle) {
		dlclose(lib->handle);
		lib->handle = NULL;
	}
	return res;
}

boolean reinitialize_gpulib(GKNVMLLib *lib)
{
	shutdown_gpulib(lib);
	return initialize_gpulib(lib);
}
