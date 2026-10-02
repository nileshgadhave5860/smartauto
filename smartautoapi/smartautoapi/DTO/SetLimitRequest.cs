using System.Text.Json.Serialization;

namespace smartautoapi.DTO
{
    public class SetLimitRequest
    {
        public int Device { get; set; }

        [JsonPropertyName("R")]
        public PhaseLimit R { get; set; } = new();

        [JsonPropertyName("B")]
        public PhaseLimit B { get; set; } = new();

        [JsonPropertyName("Y")]
        public PhaseLimit Y { get; set; } = new();
    }

    public class PhaseLimit
    {
        public bool FlagVol { get; set; }

        public decimal MinVol { get; set; }
        public decimal MaxVol { get; set; }

        public bool FlagA { get; set; }

        public decimal MinA { get; set; }
        public decimal MaxA { get; set; }
    }
}
