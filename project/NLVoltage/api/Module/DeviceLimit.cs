using System.ComponentModel;

namespace Api.Module
{

    public class DeviceLimit
    {

        public bool IsAutoSinglePhase { get; set; } = false;
        public PhaseLimit L1 { get; set; } = new();
        public PhaseLimit L2 { get; set; } = new();
        public PhaseLimit L3 { get; set; } = new();
        public PhaseLimit LN { get; set; } = new();
    }

    public class PhaseLimit
    {
        [DefaultValue(false)]
        public bool IsVol { get; set; } = false;

        public decimal MinVol { get; set; }

        public decimal MaxVol { get; set; }

        [DefaultValue(false)]
        public bool IsA { get; set; } = false;

        public decimal MinA { get; set; }

        public decimal MaxA { get; set; }
    }
}