/*
 * Copyright (C) 2020 Istituto Italiano di Tecnologia (IIT)
 * All rights reserved.
 *
 * This project is dual licensed under LGPL v2.1+ or Apache License.
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *
 * This software may be modified and distributed under the terms of the
 * GNU Lesser General Public License v2.1 or any later version.
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "scenario/gazebo/Link.h"
#include "scenario/gazebo/Log.h"
#include "scenario/gazebo/Model.h"
#include "scenario/gazebo/World.h"
#include "scenario/gazebo/components/ExternalWorldWrenchCmdWithDuration.h"
#include "scenario/gazebo/components/SimulatedTime.h"
#include "scenario/gazebo/exceptions.h"
#include "scenario/gazebo/helpers.h"

#include <ignition/gazebo/Link.hh>
#include <ignition/gazebo/components/AngularAcceleration.hh>
#include <ignition/gazebo/components/AngularVelocity.hh>
#include <ignition/gazebo/components/CanonicalLink.hh>
#include <ignition/gazebo/components/Collision.hh>
#include <ignition/gazebo/components/ContactSensorData.hh>
#include <ignition/gazebo/components/Inertial.hh>
#include <ignition/gazebo/components/LinearAcceleration.hh>
#include <ignition/gazebo/components/LinearVelocity.hh>
#include <ignition/gazebo/components/ParentEntity.hh>
#include <ignition/gazebo/components/Pose.hh>
#include <ignition/math/Inertial.hh>
#include <ignition/math/Pose3.hh>
#include <ignition/math/Quaternion.hh>
#include <ignition/math/Vector3.hh>
#include <ignition/msgs/contacts.pb.h>

#include <cassert>
#include <chrono>
#include <optional>

using namespace scenario::gazebo;

class Link::Impl
{
public:
    ignition::gazebo::Link link;

    static bool IsCanonical(const Link& link)
    {
        return link.ecm()->EntityHasComponentType(
            link.entity(),
            ignition::gazebo::components::CanonicalLink().TypeId());
    }
};

Link::Link()
    : pImpl{std::make_unique<Impl>()}
{}

Link::~Link() = default;

uint64_t Link::id() const
{
    // Get the parent world
    core::WorldPtr parentWorld = utils::getParentWorld(*this);
    assert(parentWorld);

    // Get the parent model
    core::ModelPtr parentModel = utils::getParentModel(*this);
    assert(parentModel);

    // Build a unique string identifier of this joint
    std::string scopedLinkName =
        parentWorld->name() + "::" + parentModel->name() + "::" + this->name();

    // Return the hashed string
    return std::hash<std::string>{}(scopedLinkName);
}

bool Link::initialize(const ignition::gazebo::Entity linkEntity,
                      ignition::gazebo::EntityComponentManager* ecm,
                      ignition::gazebo::EventManager* eventManager)
{
    if (linkEntity == ignition::gazebo::kNullEntity || !ecm || !eventManager) {
        sError << "Failed to initialize Link" << std::endl;
        return false;
    }

    m_ecm = ecm;
    m_entity = linkEntity;
    m_eventManager = eventManager;

    pImpl->link = ignition::gazebo::Link(linkEntity);

    // Check that the link is valid
    if (!pImpl->link.Valid(*ecm)) {
        sError << "The link entity is not valid" << std::endl;
        return false;
    }

    return true;
}

bool Link::createECMResources()
{
    sMessage << "  [" << m_entity << "] " << this->name() << std::endl;

    using namespace ignition::gazebo;

    // Create link components
    m_ecm->CreateComponent(m_entity, //
                           components::WorldPose());
    m_ecm->CreateComponent(m_entity, components::WorldLinearVelocity());
    m_ecm->CreateComponent(m_entity, components::WorldAngularVelocity());
    m_ecm->CreateComponent(m_entity, components::WorldLinearAcceleration());
    m_ecm->CreateComponent(m_entity, components::WorldAngularAcceleration());
    m_ecm->CreateComponent(m_entity, components::LinearVelocity());
    m_ecm->CreateComponent(m_entity, components::AngularVelocity());
    m_ecm->CreateComponent(m_entity, components::LinearAcceleration());
    m_ecm->CreateComponent(m_entity, components::AngularAcceleration());

    if (!this->enableContactDetection(true)) {
        sError << "Failed to enable contact detection" << std::endl;
        return false;
    }

    return true;
}

bool Link::valid() const
{
    return this->validEntity() && pImpl->link.Valid(*m_ecm);
}

std::string Link::name(const bool scoped) const
{
    auto linkNameOptional = pImpl->link.Name(*m_ecm);

    if (!linkNameOptional) {
        throw exceptions::LinkError("Failed to get link name");
    }

    std::string linkName = linkNameOptional.value();

    if (scoped) {
        linkName = utils::getParentModel(*this)->name() + "::" + linkName;
    }

    return linkName;
}

double Link::mass() const
{
    auto inertial = utils::getExistingComponentData< //
        ignition::gazebo::components::Inertial>(m_ecm, m_entity);

    return inertial.MassMatrix().Mass();
}

std::array<double, 3> Link::position() const
{
    ignition::math::Pose3d linkPose;

    if (!Impl::IsCanonical(*this)) {
        auto linkPoseOptional = pImpl->link.WorldPose(*m_ecm);

        if (!linkPoseOptional.has_value()) {
            throw exceptions::LinkError("Failed to get world position", name());
        }

        linkPose = linkPoseOptional.value();
    }
    else {
        auto parentModelOptional = pImpl->link.ParentModel(*m_ecm);
        assert(parentModelOptional.has_value());

        ignition::gazebo::Model parentModel = parentModelOptional.value();
        ignition::gazebo::Entity parentModelEntity = parentModel.Entity();

        auto W_H_M = utils::getExistingComponentData< //
            ignition::gazebo::components::Pose>(m_ecm, parentModelEntity);

        auto M_H_B = utils::getExistingComponentData< //
            ignition::gazebo::components::Pose>(m_ecm, m_entity);

        linkPose = W_H_M * M_H_B;
    }

    return utils::fromIgnitionPose(linkPose).position;
}

std::array<double, 4> Link::orientation() const
{
    ignition::math::Pose3d linkPose;

    if (!Impl::IsCanonical(*this)) {
        auto linkPoseOptional = pImpl->link.WorldPose(*m_ecm);

        if (!linkPoseOptional.has_value()) {
            throw exceptions::LinkError("Failed to get world position", name());
        }

        linkPose = linkPoseOptional.value();
    }
    else {
        auto parentModelOptional = pImpl->link.ParentModel(*m_ecm);
        assert(parentModelOptional.has_value());

        ignition::gazebo::Model parentModel = parentModelOptional.value();
        ignition::gazebo::Entity parentModelEntity = parentModel.Entity();

        auto W_H_M = utils::getExistingComponentData< //
            ignition::gazebo::components::Pose>(m_ecm, parentModelEntity);

        auto M_H_B = utils::getExistingComponentData< //
            ignition::gazebo::components::Pose>(m_ecm, m_entity);

        linkPose = W_H_M * M_H_B;
    }

    return utils::fromIgnitionPose(linkPose).orientation;
}

std::array<double, 3> Link::worldLinearVelocity() const
{
    auto linkLinearVelocity = pImpl->link.WorldLinearVelocity(*m_ecm);

    if (!linkLinearVelocity) {
        throw exceptions::LinkError("Failed to get linear velocity",
                                    this->name());
    }

    return utils::fromIgnitionVector(linkLinearVelocity.value());
}

std::array<double, 3> Link::worldAngularVelocity() const
{
    auto linkAngularVelocity = pImpl->link.WorldAngularVelocity(*m_ecm);

    if (!linkAngularVelocity) {
        throw exceptions::LinkError("Failed to get angular velocity",
                                    this->name());
    }

    return utils::fromIgnitionVector(linkAngularVelocity.value());
}

std::array<double, 3> Link::bodyLinearVelocity() const
{
    auto linkBodyLinVel = utils::getComponentData< //
        ignition::gazebo::components::LinearVelocity>(m_ecm, m_entity);

    return utils::fromIgnitionVector(linkBodyLinVel);
}

std::array<double, 3> Link::bodyAngularVelocity() const
{
    auto linkBodyAngVel = utils::getComponentData< //
        ignition::gazebo::components::AngularVelocity>(m_ecm, m_entity);

    return utils::fromIgnitionVector(linkBodyAngVel);
}

std::array<double, 3> Link::worldLinearAcceleration() const
{
    auto linkLinearAcceleration = pImpl->link.WorldLinearAcceleration(*m_ecm);

    if (!linkLinearAcceleration) {
        throw exceptions::LinkError("Failed to get linear acceleration",
                                    this->name());
    }

    return utils::fromIgnitionVector(linkLinearAcceleration.value());
}

std::array<double, 3> Link::worldAngularAcceleration() const
{
    auto linkWorldAngAcc = utils::getComponentData< //
        ignition::gazebo::components::WorldAngularAcceleration>(m_ecm,
                                                                m_entity);

    return utils::fromIgnitionVector(linkWorldAngAcc);
}

std::array<double, 3> Link::bodyLinearAcceleration() const
{
    auto linkBodyLinAcc = utils::getComponentData< //
        ignition::gazebo::components::LinearAcceleration>(m_ecm, m_entity);

    return utils::fromIgnitionVector(linkBodyLinAcc);
}

std::array<double, 3> Link::bodyAngularAcceleration() const
{
    auto linkBodyAngAcc = utils::getComponentData< //
        ignition::gazebo::components::AngularAcceleration>(m_ecm, m_entity);

    return utils::fromIgnitionVector(linkBodyAngAcc);
}

bool Link::contactsEnabled() const
{
    // Here we return true only if contacts are enables on all
    // link's collision elements;
    bool enabled = true;

    auto collisionEntities = m_ecm->ChildrenByComponents(
        m_entity,
        ignition::gazebo::components::Collision(),
        ignition::gazebo::components::ParentEntity(m_entity));

    // Create the contact sensor data component that enables the Physics
    // system to extract contact information from the physics engine
    for (const auto collisionEntity : collisionEntities) {
        bool hasContactSensorData = m_ecm->EntityHasComponentType(
            collisionEntity,
            ignition::gazebo::components::ContactSensorData().TypeId());
        enabled = enabled && hasContactSensorData;
    }

    return enabled;
}

bool Link::enableContactDetection(const bool enable)
{
    if (enable && !this->contactsEnabled()) {
        // Get all the collision entities of this link
        auto collisionEntities = m_ecm->ChildrenByComponents(
            m_entity,
            ignition::gazebo::components::Collision(),
            ignition::gazebo::components::ParentEntity(m_entity));

        // Create the contact sensor data component that enables the Physics
        // system to extract contact information from the physics engine
        for (const auto collisionEntity : collisionEntities) {
            m_ecm->CreateComponent(
                collisionEntity,
                ignition::gazebo::components::ContactSensorData());
        }

        return true;
    }

    if (!enable && this->contactsEnabled()) {
        // Get all the collision entities of this link
        auto collisionEntities = m_ecm->ChildrenByComponents(
            m_entity,
            ignition::gazebo::components::Collision(),
            ignition::gazebo::components::ParentEntity(m_entity));

        // Delete the contact sensor data component
        for (const auto collisionEntity : collisionEntities) {
            m_ecm->RemoveComponent<
                ignition::gazebo::components::ContactSensorData>(
                collisionEntity);
        }

        return true;
    }

    return true;
}

bool Link::inContact() const
{
    return this->contacts().empty() ? false : true;
}

std::vector<scenario::core::Contact> Link::contacts() const
{
    std::vector<ignition::gazebo::Entity> collisionEntities;

    // Get all the collision entities associated with this link
    m_ecm->Each<ignition::gazebo::components::Collision,
                ignition::gazebo::components::ContactSensorData,
                ignition::gazebo::components::ParentEntity>(
        [&](const ignition::gazebo::Entity& collisionEntity,
            ignition::gazebo::components::Collision*,
            ignition::gazebo::components::ContactSensorData*,
            ignition::gazebo::components::ParentEntity* parentEntityComponent)
            -> bool {
            // Keep only the collisions of this link
            if (parentEntityComponent->Data() != m_entity) {
                return true;
            }

            collisionEntities.push_back(collisionEntity);
            return true;
        });

    if (collisionEntities.empty()) {
        return {};
    }

    using BodyNameA = std::string;
    using BodyNameB = std::string;
    using CollisionsInContact = std::pair<BodyNameA, BodyNameB>;
    auto contactsMap = std::map<CollisionsInContact, core::Contact>();

    for (const auto collisionEntity : collisionEntities) {

        // Get the contact data for the selected collision entity
        const ignition::msgs::Contacts& contactSensorData =
            utils::getExistingComponentData< //
                ignition::gazebo::components::ContactSensorData>(
                m_ecm, collisionEntity);

        // Convert the ignition msg
        std::vector<core::Contact> collisionContacts =
            utils::fromIgnitionContactsMsgs(m_ecm, contactSensorData);
        //        assert(collisionContacts.size() <= 1);

        for (const auto& contact : collisionContacts) {

            assert(!contact.bodyA.empty());
            assert(!contact.bodyB.empty());

            auto key = std::make_pair(contact.bodyA, contact.bodyB);

            if (contactsMap.find(key) != contactsMap.end()) {
                contactsMap.at(key).points.insert(
                    contactsMap.at(key).points.end(),
                    contact.points.begin(),
                    contact.points.end());
            }
            else {
                contactsMap[key] = contact;
            }
        }
    }

    // Move data from the map to the output vector
    std::vector<core::Contact> allContacts;
    allContacts.reserve(contactsMap.size());

    for (auto& [_, contact] : contactsMap) {
        allContacts.push_back(std::move(contact));
    }

    return allContacts;
}

std::array<double, 6> Link::contactWrench() const
{
    auto totalForce = ignition::math::Vector3d::Zero;
    auto totalTorque = ignition::math::Vector3d::Zero;

    const auto& contacts = this->contacts();

    for (const auto& contact : contacts) {
        // Each contact wrench is expressed with respect to the contact point
        // and with the orientation of the world frame. We need to translate it
        // to the link frame.

        for (const auto& contactPoint : contact.points) {
            // The contact points extracted from the physics do not have torque
            constexpr std::array<double, 3> zero = {0, 0, 0};
            assert(contactPoint.torque == zero);

            // Link position
            const auto& o_L = utils::toIgnitionVector3(this->position());

            // Contact position
            const auto& o_P = utils::toIgnitionVector3(contactPoint.position);

            // Relative position
            const auto L_o_P = o_P - o_L;

            // The contact force and the total link force are both expressed
            // with the orientation of the world frame. This simplifies the
            // conversion since we have to take into account only the
            // displacement.
            auto force = utils::toIgnitionVector3(contactPoint.force);

            // The force does not have to be changed
            totalForce += force;

            // There is however a torque that balances out the resulting moment
            totalTorque += L_o_P.Cross(force);
        }
    }

    return {totalForce[0],
            totalForce[1],
            totalForce[2],
            totalTorque[0],
            totalTorque[1],
            totalTorque[2]};
}

bool Link::applyWorldForce(const std::array<double, 3>& force,
                           const double duration)
{
    return this->applyWorldWrench(force, {0, 0, 0}, duration);
}

bool Link::applyWorldTorque(const std::array<double, 3>& torque,
                            const double duration)
{
    return this->applyWorldWrench({0, 0, 0}, torque, duration);
}

bool Link::applyWorldWrench(const std::array<double, 3>& force,
                            const std::array<double, 3>& torque,
                            const double duration)
{
    // Adapted from ignition::gazebo::Link::AddWorld{Force,Wrench}
    using namespace std::chrono;
    using namespace ignition;
    using namespace ignition::gazebo;

    auto inertial =
        utils::getExistingComponentData<components::Inertial>(m_ecm, m_entity);

    auto worldPose =
        utils::getExistingComponentData<components::WorldPose>(m_ecm, m_entity);

    auto forceIgnitionMath = utils::toIgnitionVector3(force);

    // We want the force to be applied at the center of mass, but
    // ExternalWorldWrenchCmd applies the force at the link origin so we need to
    // compute the resulting force and torque on the link origin.

    // Compute W_o_I = W_R_L * L_o_I
    auto linkCOMInWorldCoordinates =
        worldPose.Rot().RotateVector(inertial.Pose().Pos());

    // Initialize the torque with the argument
    auto torqueIgnitionMath = utils::toIgnitionVector3(torque);

    // Sum the component given by the projection of the force to the link origin
    torqueIgnitionMath += linkCOMInWorldCoordinates.Cross(forceIgnitionMath);

    // Get the current simulated time
    auto& now = utils::getExistingComponentData<components::SimulatedTime>(
        m_ecm,
        utils::getFirstParentEntityWithComponent<components::SimulatedTime>(
            m_ecm, m_entity));

    // Create a new wrench with duration
    utils::WrenchWithDuration wrench(
        forceIgnitionMath,
        torqueIgnitionMath,
        utils::doubleToSteadyClockDuration(duration),
        now);

    utils::LinkWrenchCmd& linkWrenchCmd =
        utils::getComponentData<components::ExternalWorldWrenchCmdWithDuration>(
            m_ecm, m_entity);

    linkWrenchCmd.addWorldWrench(wrench);
    return true;
}
