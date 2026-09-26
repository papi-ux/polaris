class PolarisVersion {
  constructor(release = null, version = null) {
    if (release) {
      this.release = release;
      this.version = release.tag_name;
      this.versionName = release.name;
      this.versionTag = release.tag_tag;
    } else if (version) {
      this.release = null;
      this.version = version;
      this.versionName = null;
      this.versionTag = null;
    } else {
      throw new Error('Either release or version must be provided');
    }
    this.versionParts = this.parseVersion(this.version);
    this.versionMajor = this.versionParts ? this.versionParts[0] : null;
    this.versionMinor = this.versionParts ? this.versionParts[1] : null;
    this.versionPatch = this.versionParts ? this.versionParts[2] : null;
    this.versionIncremental = this.versionParts ? this.versionParts[3] : null;
  }

  parseVersion(version) {
    if (!version) {
      return null;
    }
    let v = version;
    if (v.indexOf("v") === 0) {
      v = v.substring(1);
    }

    const match = v.match(/^(\d+)\.(\d+)\.(\d+)(?:-([^+]+))?(?:\+[^+]+)?$/);
    if (!match) return null;
    const versionParts = match.slice(1, 4).map(Number);
    if (!versionParts.every(Number.isSafeInteger)) return null;
    const suffix = match[4];
    // Compare channel and sequence separately: rc.1 outranks every beta, and
    // the stable release outranks its rc builds. This agrees with Nova's bands
    // without bitwise overflow or imposing Android's versionCode limits here.
    if (!suffix) versionParts.push(4, 0);
    else if (suffix === 'pre') versionParts.push(0, 0);
    else {
      const prerelease = suffix.match(/^(alpha|beta|rc)\.(\d+)$/);
      if (prerelease && Number.isSafeInteger(Number(prerelease[2]))) {
        versionParts.push({ alpha: 1, beta: 2, rc: 3 }[prerelease[1]], Number(prerelease[2]));
      }
      // Unknown development suffixes retain release-number-only comparison.
    }

    return versionParts;
  }

  isGreater(otherVersion, checkIncremental) {
    let otherVersionParts;
    if (otherVersion instanceof PolarisVersion) {
      otherVersionParts = otherVersion.versionParts;
    } else if (typeof otherVersion === 'string') {
      otherVersionParts = this.parseVersion(otherVersion);
    } else {
      throw new Error('Invalid argument: otherVersion must be a PolarisVersion object or a version string');
    }

    if (!this.versionParts || !otherVersionParts) {
      return false;
    }
    for (let i = 0; i < Math.min(checkIncremental ? 5 : 3, this.versionParts.length, otherVersionParts.length); i++) {
      if (this.versionParts[i] > otherVersionParts[i]) {
        return true;
      } else if (this.versionParts[i] < otherVersionParts[i]) {
        return false;
      }
    }
    return false;
  }
}

export default PolarisVersion;
