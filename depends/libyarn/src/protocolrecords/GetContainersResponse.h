/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#ifndef GETCONTAINERSRESPONSE_H_
#define GETCONTAINERSRESPONSE_H_

#include <list>
#include "YARN_yarn_service_protos.pb.h"
#include "records/ContainerReport.h"

using namespace std;
using namespace hadoop::yarn;
namespace libyarn {

/*
message GetContainersResponseProto {
  repeated ContainerReportProto containers_reports = 1;
}
*/

class GetContainersResponse {
public:
	GetContainersResponse();
	GetContainersResponse(const GetContainersResponseProto &proto);
	virtual ~GetContainersResponse();

	GetContainersResponseProto& getProto();

	list<ContainerReport> getContainersReportList();
	void setContainersReportList(list<ContainerReport> &containersReport);

private:
	GetContainersResponseProto responseProto;
};

} /* namespace libyarn */

#endif /* GETCONTAINERSRESPONSE_H_ */
