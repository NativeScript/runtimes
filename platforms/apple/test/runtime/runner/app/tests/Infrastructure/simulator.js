function isOperatingSystemAtLeastVersion(majorVersion, minorVersion, patchVersion) {
    var processInfo = NSProcessInfo.processInfo;
    if (!processInfo) {
        return false;
    }

    if (typeof processInfo.isOperatingSystemAtLeastVersion === "function") {
        return processInfo.isOperatingSystemAtLeastVersion({
            majorVersion: majorVersion,
            minorVersion: minorVersion,
            patchVersion: patchVersion
        });
    }

    var version = processInfo.operatingSystemVersion;
    if (!version) {
        return false;
    }

    if (version.majorVersion !== majorVersion) {
        return version.majorVersion > majorVersion;
    }

    if (version.minorVersion !== minorVersion) {
        return version.minorVersion > minorVersion;
    }

    return version.patchVersion >= patchVersion;
}

function isSimulator() {
    if (isOperatingSystemAtLeastVersion(9, 0, 0)) {
        return NSProcessInfo.processInfo.environment.objectForKey("SIMULATOR_DEVICE_NAME") !== null;
    } else {
        return UIDevice.currentDevice.name.toLowerCase().indexOf("simulator") > -1;
    }
}

global.isSimulator = isSimulator();
