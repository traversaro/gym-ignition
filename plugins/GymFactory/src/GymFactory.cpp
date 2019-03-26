/*
 * Copyright (C) 2019 Istituto Italiano di Tecnologia (IIT)
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms of the
 * GNU Lesser General Public License v2.1 or any later version.
 */

#include "gympp/GymFactory.h"
#include "gympp/Log.h"
#include "gympp/gazebo/IgnitionEnvironment.h"

#include <cassert>

using namespace gympp;

class GymFactory::Impl
{
public:
    inline bool exists(const EnvironmentName& name) const
    {
        return plugins.find(name) != plugins.end();
    }
    spaces::SpacePtr makeSpace(const SpaceMetadata& md);
    std::unordered_map<EnvironmentName, const PluginMetadata> plugins;
};

gympp::spaces::SpacePtr gympp::GymFactory::Impl::makeSpace(const SpaceMetadata& md)
{
    assert(md.isValid());
    gympp::spaces::SpacePtr space;

    switch (md.type) {
        case gympp::SpaceType::Box: {
            if (md.dims.empty()) {
                space = std::make_shared<gympp::spaces::Box>(md.low, md.high);
            }
            else {
                space = std::make_shared<gympp::spaces::Box>(md.low[0], md.high[0], md.dims);
            }
            break;
        }
        case gympp::SpaceType::Discrete: {
            space = std::make_shared<gympp::spaces::Discrete>(md.dims[0]);
            break;
        }
    }

    return space;
}

gympp::GymFactory::GymFactory()
    : pImpl{new Impl(), [](Impl* impl) { delete impl; }}
{}

gympp::EnvironmentPtr gympp::GymFactory::make(const std::__cxx11::string& envName)
{
    if (!pImpl->exists(envName)) {
        gymppError << "Environment '" << envName << "' has never been registered" << std::endl;
        return nullptr;
    }

    auto& md = pImpl->plugins[envName];

    assert(md.isValid());
    auto actionSpace = pImpl->makeSpace(md.actionSpace);
    auto observationSpace = pImpl->makeSpace(md.observationSpace);

    if (!actionSpace || !observationSpace) {
        gymppError << "Failed to create spaces" << std::endl;
        return nullptr;
    }

    // Create the environment
    auto ignGym = std::make_shared<gazebo::IgnitionEnvironment>(actionSpace,
                                                                observationSpace,
                                                                /*updateRate=*/50,
                                                                /*iterations=*/10);

    // Setup the CartPolePlugin
    if (!ignGym->setupIgnitionPlugin(md.libraryName, md.className)) {
        gymppError << "Failed to setup the ignition plugin" << std::endl;
        return nullptr;
    }

    // Setup the world
    if (!ignGym->setupGazeboWorld(md.worldFileName, md.modelNames)) {
        gymppError << "Failed to setup SDF file";
        return nullptr;
    }

    return ignGym->env();
}

bool gympp::GymFactory::registerPlugin(const PluginMetadata& md)
{
    if (!md.isValid()) {
        gymppError << "The plugin metadata is not valid" << std::endl;
        return false;
    }

    if (pImpl->exists(md.environmentName)) {
        gymppError << "Environment '" << md.environmentName << "' has been already registered"
                   << std::endl;
        return false;
    }

    pImpl->plugins.insert({md.environmentName, md});
    return true;
}