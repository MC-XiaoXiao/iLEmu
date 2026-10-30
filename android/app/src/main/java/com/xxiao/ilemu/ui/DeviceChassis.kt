package com.xxiao.ilemu.ui

/** Front-view coordinates use enclosure width as their unit, independently of touch targets. */
internal data class ChassisKey(val x: Float, val y: Float, val width: Float, val height: Float,
    val shape: KeyShape = KeyShape.Side)
internal data class ChassisCamera(val x: Float, val y: Float, val diameter: Float)
internal enum class ChassisFinish { Light, Neutral, Dark }
internal data class DeviceChassis(
    val height: Float,
    val screenWidth: Float,
    val screenTop: Float,
    val corner: Float,
    val rim: Float,
    val homeDiameter: Float,
    val finish: ChassisFinish = ChassisFinish.Light,
    val speakerY: Float? = null,
    val camera: ChassisCamera? = null,
    val power: ChassisKey = ChassisKey(.76f, -.004f, .16f, .012f, KeyShape.Top),
    val ringer: ChassisKey? = null,
    val volumeUp: ChassisKey? = null,
    val volumeDown: ChassisKey? = null,
    val antennaY: List<Float> = emptyList()
) {
    companion object {
        // Measured from the corresponding Apple Wiki front images. Shared mechanical
        // designs share geometry; firmware/app versions never select an enclosure.
        private val original = DeviceChassis(1.895f, .842f, .322f, .19f, .035f, .175f,
            speakerY = .18f,
            ringer = ChassisKey(0f, .23f, .016f, .085f),
            volumeUp = ChassisKey(0f, .42375f, .014f, .1675f, KeyShape.RockerUpper),
            volumeDown = ChassisKey(0f, .59125f, .014f, .1675f, KeyShape.RockerLower))
        private val curvedPhone = DeviceChassis(1.852f, .832f, .30f, .17f, .023f, .175f,
            speakerY = .17f,
            ringer = ChassisKey(0f, .20f, .014f, .07f),
            volumeUp = ChassisKey(0f, .39f, .012f, .12f, KeyShape.RockerUpper),
            volumeDown = ChassisKey(0f, .51f, .012f, .12f, KeyShape.RockerLower))
        private val glassPhone = DeviceChassis(1.948f, .848f, .34f, .15f, .013f, .18f,
            ChassisFinish.Neutral, speakerY = .17f, camera = ChassisCamera(.33f, .17f, .035f),
            ringer = ChassisKey(0f, .22f, .014f, .07f),
            volumeUp = ChassisKey(0f, .41f, .012f, .075f),
            volumeDown = ChassisKey(0f, .60f, .012f, .075f),
            antennaY = listOf(.16f, 1.69f))
        private val tallPhone = DeviceChassis(2.14f, .865f, .303f, .14f, .015f, .19f,
            ChassisFinish.Dark, speakerY = .18f, camera = ChassisCamera(.5f, .10f, .033f),
            ringer = ChassisKey(0f, .33f, .014f, .08f),
            volumeUp = ChassisKey(0f, .53f, .013f, .07f),
            volumeDown = ChassisKey(0f, .72f, .013f, .07f),
            antennaY = listOf(.18f, 1.94f))
        private val flatPlayer = DeviceChassis(1.78f, .822f, .28f, .13f, .033f, .17f,
            power = ChassisKey(.22f, -.004f, .17f, .012f, KeyShape.Top))
        private val curvedPlayer = DeviceChassis(1.796f, .82f, .29f, .14f, .019f, .175f,
            power = ChassisKey(.23f, -.004f, .17f, .012f, KeyShape.Top),
            volumeUp = ChassisKey(0f, .29f, .012f, .10f, KeyShape.RockerUpper),
            volumeDown = ChassisKey(0f, .39f, .012f, .10f, KeyShape.RockerLower))
        private val tablet = DeviceChassis(1.279f, .79f, .12f, .055f, .012f, .07f,
            ChassisFinish.Neutral,
            power = ChassisKey(.89f, -.004f, .09f, .009f, KeyShape.Top),
            ringer = ChassisKey(1f, .12f, .011f, .045f),
            volumeUp = ChassisKey(1f, .215f, .009f, .075f, KeyShape.RockerUpper),
            volumeDown = ChassisKey(1f, .29f, .009f, .075f, KeyShape.RockerLower))
        private val slimTablet = tablet.copy(height = 1.303f, screenWidth = .80f, screenTop = .115f,
            rim = .008f, camera = ChassisCamera(.5f, .047f, .012f))
        private val models = mapOf(
            "iPhone1,1" to original,
            "iPhone1,2" to curvedPhone,
            "iPhone2,1" to curvedPhone.copy(height = 1.856f),
            "iPhone3,1" to glassPhone,
            "iPhone4,1" to glassPhone.copy(height = 1.96f, antennaY = listOf(.10f, 1.70f)),
            "iPhone5,1" to tallPhone,
            "iPod1,1" to flatPlayer,
            "iPod2,1" to curvedPlayer,
            "iPad1,1" to tablet,
            "iPad2,1" to slimTablet)
        fun forDevice(identifier: String?) = models[identifier] ?: original
    }
}
