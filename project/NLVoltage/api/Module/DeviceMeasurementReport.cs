using System.Text.Json.Serialization;

namespace Api.Module;

public sealed record DevicePhaseReading(
	[property: JsonPropertyName("VName")] string VName,
	[property: JsonPropertyName("VValue")] decimal VValue,
	[property: JsonPropertyName("VStatus")] int VStatus,
	[property: JsonPropertyName("AValue")] decimal AValue,
	[property: JsonPropertyName("AStatus")] int AStatus);