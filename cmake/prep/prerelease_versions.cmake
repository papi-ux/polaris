# Runtime and package versions for a prerelease build.
#
# A release tag vX.Y.Z-beta.N or vX.Y.Z-rc.N reaches the build as the label beta.N or rc.N, and a
# stable tag reaches it as no label. Every package manager has to sort a prerelease below the release
# of the same number, and the prereleases in order among themselves, or an ordinary upgrade to that
# release leaves the prerelease in place. The v1.4.13 betas carried 1.4.13 itself, and dnf answered
# the 1.4.13 release with "Nothing to do". Each format needs its own spelling:
#
#   runtime  X.Y.Z-beta.N  semver: what the host reports and the Update Center compares.
#   RPM      X.Y.Z~beta.N  rpm sorts a tilde below anything, even the end of the version, so below
#                          X.Y.Z. CPack turns a hyphen in the version into an underscore, and rpm
#                          sorts that above X.Y.Z.
#   DEB      X.Y.Z~beta.N  the same in dpkg, which reads a hyphen as the Debian revision and so
#                          sorts X.Y.Z-beta.N above X.Y.Z.
#   pacman   X.Y.Zbeta.N   beta.N or rc.N joined straight onto the number sorts below it. pacman
#                          sorts a tilde, an underscore or a dot there above X.Y.Z, and makepkg
#                          refuses a hyphen in pkgver.
#
# Each format orders 1.4.12, 1.4.13 beta.2, beta.3, beta.10, rc.1, rc.2, 1.4.13, 1.4.14 beta.1 and
# 1.4.14 strictly ascending; tests/scripts/test_prerelease_package_versions.py pins the mapping and
# checks that order with each package manager it finds. A stable build keeps exactly the versions
# it always had: RPM X.Y.Z-1, DEB X.Y.Z, pacman X.Y.Z-1.
#
# polaris_prerelease_versions(<version> <label>) sets, in the caller's scope:
#   POLARIS_RUNTIME_VERSION      what the host reports
#   POLARIS_RPM_PACKAGE_VERSION  the RPM Version; the Release stays 1
#   POLARIS_DEB_PACKAGE_VERSION  the DEB Version, with no Debian revision
#   POLARIS_SUB_VERSION          what the PKGBUILDs append to pkgver; pkgrel stays 1
#   POLARIS_PACMAN_PKGVER        the pkgver the PKGBUILDs end up with
function(polaris_prerelease_versions version label)
    # Only a release number with a channel stuck onto it is refused. Other callers pass versions of
    # their own, such as the Nix package's 0-unstable-DATE, and those go through untouched.
    if(version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+[-~_.]?(alpha|beta|rc)")
        message(FATAL_ERROR
                "Polaris version '${version}' already carries a prerelease suffix. Pass the release "
                "number as BUILD_VERSION and the label as POLARIS_PRERELEASE_LABEL, so each package "
                "format can spell it the way it sorts below that release.")
    endif()
    if(label STREQUAL "")
        set(runtime "${version}")
        set(package "${version}")
        set(sub "")
    else()
        if(NOT label MATCHES "^(beta|rc)\\.[0-9]+$")
            message(FATAL_ERROR "POLARIS_PRERELEASE_LABEL must be beta.N or rc.N, not '${label}'")
        endif()
        if(NOT version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
            message(FATAL_ERROR
                    "POLARIS_PRERELEASE_LABEL '${label}' needs a plain release version from BRANCH and "
                    "BUILD_VERSION, not '${version}'")
        endif()
        set(runtime "${version}-${label}")
        set(package "${version}~${label}")
        set(sub "${label}")
    endif()
    set(POLARIS_RUNTIME_VERSION "${runtime}" PARENT_SCOPE)
    set(POLARIS_RPM_PACKAGE_VERSION "${package}" PARENT_SCOPE)
    set(POLARIS_DEB_PACKAGE_VERSION "${package}" PARENT_SCOPE)
    set(POLARIS_SUB_VERSION "${sub}" PARENT_SCOPE)
    set(POLARIS_PACMAN_PKGVER "${version}${sub}" PARENT_SCOPE)
endfunction()
