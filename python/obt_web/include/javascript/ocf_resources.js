
/*These devices have a valid image 
 * /include/images
 * */
var images = [
	'oic.d.diplomat',
	'oic.d.light',
	'oic.d.phone',
	'oic.d.speaker'
]

/*Input device
 * Returns: array of device types
 * */
function get_device_types(device){
	var device_types =[];
	for (resource in device.resources){
		if(device.resources[resource].uri.includes("/oic/d")){
			for(type_index in device.resources[resource].types){
				if(!device.resources[resource].types[type_index].includes("oic.wk.d")){
					device_types.push(device.resources[resource].types[type_index]);
				}
			}
		}
	}
	return(device_types)
}


/*Returns: HTML based controls depending on the device type
 * */
function return_client_controls(device){

	var device_types = get_device_types(device);	
	var client_controls = "";
	for(type_index in device_types){
		device_type = device_types[type_index];
		switch(device_type){
		case "oic.d.light":
			var cmd = 'post';
			client_controls += `
			 <div>`+device_type+`</div>
			  <div>
				  <label class="switch">
				  <input type="checkbox" id='switch_`+device.uuid+`' onchange=send_command(this,'`+device_type+`','`+cmd+`')>
				  <span class="slider round"></span>
				</label>
			  </div>
			`;
			continue;
		case "oic.d.diplomat":
			client_controls += `
			 <div>Streamlined Onboarding</div>
			  <div>
				<label class="switch">
				  <input type="checkbox" id='switch_so' onchange=observe_diplomat(this)>
				  <span class="slider round"></span>
				</label>
			  </div>
			`;
			break;
		default:
			client_controls = `
			<div class='no_resource'>
				No resources returned
				</div>
			`;
			break;

		}
	}

	return (client_controls);

}
/*
 *
	  <p>
		 /a/light
	  </p>
		  <label class="switch">
		  <input type="checkbox" id='switch_`+uuid+`' onchange=send_command(this)>
		  <span class="slider round"></span>
		</label>
	  </p>
	  <p>
	  /a/brightlight
	  </p>
	  <p>
		<div class="slidecontainer">
		  <input type="range" min="1" max="100" value="50">
		</div>
	  </p>
	  */
