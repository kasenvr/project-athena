//
//  audioBall.js
//  hifi
//
//  Created by Athanasios Gaitatzes on 2/10/14.
//  Copyright (c) 2014 HighFidelity, Inc. All rights reserved.
//
//  This script creates a particle in front of the user that stays in front of
//  the user's avatar as they move, and animates it's radius and color
//  in response to the audio intensity.
//

// add two vectors
function vPlus(a, b) { 
    var rval = { x: a.x + b.x, y: a.y + b.y, z: a.z + b.z };
    return rval;
}

// multiply scalar with vector
function vsMult(s, v) {
    var rval = { x: s * v.x, y: s * v.y, z: s * v.z };
    return rval;
}

var sound = new Sound("https://s3-us-west-1.amazonaws.com/highfidelity-public/sounds/Animals/mexicanWhipoorwill.raw");
var FACTOR = 0.75;

function addParticle()
{
    // the particle should be placed in front of the user's avatar
    var avatarFront = Quat.getFront(MyAvatar.orientation);

    // move particle three units in front of the avatar
    var particlePosition = vPlus(MyAvatar.position, vsMult (3, avatarFront));

    // play a sound at the location of the particle
    var options = new AudioInjectionOptions();
    options.position = particlePosition;
    options.volume = 0.25;
    Audio.playSound(sound, options);

    var audioAverageLoudness = MyAvatar.audioAverageLoudness * FACTOR;
    //print ("Audio Loudness = " + MyAvatar.audioLoudness + " -- Audio Average Loudness = " + MyAvatar.audioAverageLoudness);

    // animates the particles radius and color in response to the changing audio intensity
    var particleProperies = {
        position: particlePosition // the particle should stay in front of the user's avatar as he moves
    ,   color: { red: 0, green: 255 * audioAverageLoudness, blue: 0 }
    ,   radius: audioAverageLoudness
    ,   velocity: { x: 0.0, y: 0.0, z: 0.0 }
    ,   gravity: { x: 0.0, y: 0.0, z: 0.0 }
    ,   damping: 0.0
    ,   lifetime: 0.05
    }

    Particles.addParticle (particleProperies);
}

// register the call back so it fires before each data send
Script.willSendVisualDataCallback.connect(addParticle);

// register our scriptEnding callback
Script.scriptEnding.connect(function scriptEnding() {});
