using System.ComponentModel;

namespace Api.Module
{

    public class DeviceLimit
    {
        public PhaseLimit N { get; set; } = new();
        public PhaseLimit L { get; set; } = new();
        public PhaseLimit R { get; set; } = new();

        public PhaseLimit B { get; set; } = new();

        public PhaseLimit Y { get; set; } = new();
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