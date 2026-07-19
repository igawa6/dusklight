#ifndef DUSK_APPNAME_HPP
#define DUSK_APPNAME_HPP

namespace dusk {
    /**
     * \brief The internal application name for the game.
     *
     * This gets used for file paths and such, and cannot be changed!
     */
    constexpr auto AppName = "Dusklight";

    /**
     * Previous AppName to migrate data from.
     */
    constexpr auto LegacyAppName = "Dusk";

    /**
     * \brief The internal organization name for the game.
     *
     * This gets used for file paths and such, and cannot be changed!
     */
    constexpr auto OrgName = "TwilitRealm";

    /**
     * \brief GitHub owner/repo the startup update check queries.
     *
     * Deliberately separate from OrgName: that one is a file-path identity
     * and must not change, while this tracks whichever fork ships the build.
     */
    constexpr auto UpdateRepoOwner = "igawa6";
    constexpr auto UpdateRepoName = "dusklight";
}

#endif  // DUSK_APPNAME_HPP
